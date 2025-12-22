#include <string>
#include <map>
#include <cstring>

#include "esp_log.h"

extern "C" {
    #include <cJSON.h>
    #include "esp_http_server.h"
}

#include "http_server.h"
#include "http_requester.h"
#include "cache_lights.h"

static const char* TAG = "HTTP_SERVER";

// Add a cached JSON response that's updated periodically
struct ServerCache {
    std::string cached_devices_json;
    SemaphoreHandle_t mutex;

    ServerCache() {
        mutex = xSemaphoreCreateMutex();
        cached_devices_json = "[]";
    }
};

struct ServerContext {
    const std::map<std::string, DeviceInfo>* device_map;
    const std::map<std::string, DeviceInfo>* device_serial_map;
    LightGroupCache* light_group_cache;
};

static ServerCache* server_cache = nullptr;

// --- Utility Functions ---

/**
 * @brief Converts the device map to a JSON string.
 */
static std::string device_map_to_json(const std::map<std::string, DeviceInfo> *device_map) {
    cJSON *root = cJSON_CreateArray();

    for (const auto& pair : *device_map) {
        const DeviceInfo& info = pair.second;

        cJSON *device = cJSON_CreateObject();
        cJSON_AddStringToObject(device, "serialNumber", info.serialNumber.c_str());
        cJSON_AddStringToObject(device, "ip", info.ip.c_str());
        cJSON_AddStringToObject(device, "productName", info.productName.c_str());
        cJSON_AddNumberToObject(device, "hardwareBoardType", info.hardwareBoardType);
        cJSON_AddStringToObject(device, "hardwareRevision", info.hardwareRevision.c_str());
        cJSON_AddStringToObject(device, "macAddress", info.macAddress.c_str());
        cJSON_AddNumberToObject(device, "firmwareBuildNumber", info.firmwareBuildNumber);
        cJSON_AddStringToObject(device, "firmwareVersion", info.firmwareVersion.c_str());
        cJSON_AddStringToObject(device, "displayName", info.displayName.c_str());

        cJSON_AddItemToArray(root, device);
    }

    char *json_string = cJSON_Print(root);
    std::string result(json_string);

    cJSON_free(json_string);
    cJSON_Delete(root);

    return result;
}

// --- Route Handler Functions ---


/**
 * @brief Handler for GET /lights/all - returns all discovered devices.
 * Now uses cached response for instant, non-blocking replies.
 */
static esp_err_t handleGetAllLights(httpd_req_t *req) {
    if (!server_cache) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "{\"error\":\"Server cache not initialized\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Use cached JSON instead of generating on-the-fly
    std::string json;
    if (xSemaphoreTake(server_cache->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        json = server_cache->cached_devices_json;
        xSemaphoreGive(server_cache->mutex);
    } else {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "{\"error\":\"Cache busy\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());

    return ESP_OK;
}

/**
 * @brief Handler for GET /lights/group - returns all light groups.
 */
static esp_err_t handleGetLightGroups(httpd_req_t *req) {
    ServerContext* ctx = (ServerContext*)req->user_ctx;

    // Get all groups from cache
    std::map<std::string, std::vector<std::string>> allGroups = ctx->light_group_cache->getAllGroups();

    // Build JSON response
    cJSON *root = cJSON_CreateObject();
    cJSON *groupsArray = cJSON_CreateArray();

    for (const auto& groupPair : allGroups) {
        cJSON *groupObj = cJSON_CreateObject();
        cJSON_AddStringToObject(groupObj, "groupName", groupPair.first.c_str());

        cJSON *serialsArray = cJSON_CreateArray();
        for (const auto& serial : groupPair.second) {
            cJSON_AddItemToArray(serialsArray, cJSON_CreateString(serial.c_str()));
        }

        cJSON_AddItemToObject(groupObj, "serialNumbers", serialsArray);
        cJSON_AddNumberToObject(groupObj, "deviceCount", groupPair.second.size());
        cJSON_AddItemToArray(groupsArray, groupObj);
    }

    cJSON_AddItemToObject(root, "groups", groupsArray);
    cJSON_AddNumberToObject(root, "totalGroups", allGroups.size());

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    cJSON_free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

/**
 * @brief Handler for PUT /lights/group - creates or updates a light group.
 * Expects JSON body: {"groupName": "...", "serialNumbers": ["...", ...]}
 */
static esp_err_t handleSetLightGroup(httpd_req_t *req) {
    ServerContext* ctx = (ServerContext*)req->user_ctx;

    ESP_LOGI(TAG, "Received PUT /lights/group request");

    // Read request body
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        ESP_LOGW(TAG, "Failed to read request body");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Failed to read request body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    buf[ret] = '\0';

    ESP_LOGI(TAG, "Received JSON (%d bytes): %s", ret, buf);

    // Parse JSON
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid JSON\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "JSON parsed successfully");

    cJSON *groupName = cJSON_GetObjectItemCaseSensitive(root, "groupName");
    cJSON *serialNumbers = cJSON_GetObjectItemCaseSensitive(root, "serialNumbers");

    if (!cJSON_IsString(groupName) || !cJSON_IsArray(serialNumbers)) {
        ESP_LOGE(TAG, "Invalid groupName or serialNumbers in JSON");
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing or invalid groupName or serialNumbers\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Extracting serial numbers for group: %s", groupName->valuestring);

    // Extract serial numbers
    std::vector<std::string> serials;
    int arraySize = cJSON_GetArraySize(serialNumbers);
    ESP_LOGI(TAG, "Array size: %d", arraySize);

    for (int i = 0; i < arraySize; i++) {
        cJSON *item = cJSON_GetArrayItem(serialNumbers, i);
        if (cJSON_IsString(item) && item->valuestring != nullptr) {
            serials.push_back(std::string(item->valuestring));
            ESP_LOGI(TAG, "Added serial [%d]: %s", i, item->valuestring);
        }
    }

    if (serials.empty()) {
        ESP_LOGW(TAG, "serialNumbers array is empty");
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"serialNumbers array is empty\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "About to add group '%s' with %d devices to cache", groupName->valuestring, serials.size());

    // Save group name string before deleting root JSON object
    std::string savedGroupName = std::string(groupName->valuestring);

    // Add group to in-memory cache (without NVS save yet)
    ctx->light_group_cache->addGroup(savedGroupName, serials, false);

    ESP_LOGI(TAG, "Group added to cache successfully");

    cJSON_Delete(root);

    // Send success response BEFORE writing to NVS
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "groupName", savedGroupName.c_str());
    cJSON_AddNumberToObject(response, "deviceCount", serials.size());

    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    cJSON_free(json_str);
    cJSON_Delete(response);

    ESP_LOGI(TAG, "Response sent, now persisting to NVS");

    // Now save to NVS after response has been sent
    ctx->light_group_cache->saveToNVS();

    ESP_LOGI(TAG, "Created/updated group '%s' with %d devices", savedGroupName.c_str(), serials.size());

    return ESP_OK;
}

/**
 * @brief Handler for PUT /lights - sets light state for all devices in a group.
 * Expects JSON body: {"group": "<groupName>", "light": {"brightness": <0-100>, "temperature": <143-344>}}
 */
static esp_err_t handleControlLightGroup(httpd_req_t *req) {
    ServerContext* ctx = (ServerContext*)req->user_ctx;

    ESP_LOGI(TAG, "Received PUT /lights request");

    // Read request body
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        ESP_LOGW(TAG, "Failed to read request body");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Failed to read request body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    buf[ret] = '\0';

    ESP_LOGI(TAG, "Received JSON (%d bytes): %s", ret, buf);

    // Parse JSON
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid JSON\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cJSON *group_json = cJSON_GetObjectItemCaseSensitive(root, "group");
    cJSON *light_json = cJSON_GetObjectItemCaseSensitive(root, "light");

    if (!cJSON_IsString(group_json) || !cJSON_IsObject(light_json)) {
        ESP_LOGE(TAG, "Invalid group or light in JSON");
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing or invalid 'group' or 'light' fields\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cJSON *brightness_json = cJSON_GetObjectItemCaseSensitive(light_json, "brightness");
    cJSON *temperature_json = cJSON_GetObjectItemCaseSensitive(light_json, "temperature");

    if (!cJSON_IsNumber(brightness_json) || !cJSON_IsNumber(temperature_json)) {
        ESP_LOGE(TAG, "Invalid brightness or temperature in light object");
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing or invalid brightness or temperature in light object\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    std::string groupName = std::string(group_json->valuestring);
    int brightness = brightness_json->valueint;
    int temperature = temperature_json->valueint;

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Setting group '%s' to brightness=%d, temperature=%d", groupName.c_str(), brightness, temperature);

    // Get serial numbers for the group
    std::vector<std::string> serialNumbers = ctx->light_group_cache->getGroup(groupName);

    if (serialNumbers.empty()) {
        ESP_LOGW(TAG, "Group '%s' not found or empty", groupName.c_str());
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Group not found or empty\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Found %d devices in group '%s'", serialNumbers.size(), groupName.c_str());

    // Control each light in the group
    int successCount = 0;
    int failCount = 0;
    cJSON *results = cJSON_CreateArray();

    for (const auto& serial : serialNumbers) {
        // Look up device info by serial number
        auto it = ctx->device_serial_map->find(serial);
        if (it == ctx->device_serial_map->end()) {
            ESP_LOGW(TAG, "Serial '%s' not found in device map", serial.c_str());
            failCount++;

            cJSON *result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "serial", serial.c_str());
            cJSON_AddBoolToObject(result, "success", false);
            cJSON_AddStringToObject(result, "error", "Device not found");
            cJSON_AddItemToArray(results, result);
            continue;
        }

        const DeviceInfo& deviceInfo = it->second;
        ESP_LOGI(TAG, "Controlling light: %s (%s)", deviceInfo.displayName.c_str(), deviceInfo.ip.c_str());

        // Set light state
        ElgatoLight light = setLight(deviceInfo.ip, brightness, temperature);

        if (light.error.empty()) {
            successCount++;
            ESP_LOGI(TAG, "Successfully controlled %s", deviceInfo.displayName.c_str());

            cJSON *result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "serial", serial.c_str());
            cJSON_AddStringToObject(result, "displayName", deviceInfo.displayName.c_str());
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddNumberToObject(result, "brightness", light.brightness);
            cJSON_AddNumberToObject(result, "temperature", light.temperature);
            cJSON_AddItemToArray(results, result);
        } else {
            failCount++;
            ESP_LOGW(TAG, "Failed to control %s: %s", deviceInfo.displayName.c_str(), light.error.c_str());

            cJSON *result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "serial", serial.c_str());
            cJSON_AddStringToObject(result, "displayName", deviceInfo.displayName.c_str());
            cJSON_AddBoolToObject(result, "success", false);
            cJSON_AddStringToObject(result, "error", light.error.c_str());
            cJSON_AddItemToArray(results, result);
        }
    }

    // Build response
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "groupName", groupName.c_str());
    cJSON_AddNumberToObject(response, "totalDevices", serialNumbers.size());
    cJSON_AddNumberToObject(response, "successCount", successCount);
    cJSON_AddNumberToObject(response, "failCount", failCount);
    cJSON_AddItemToObject(response, "results", results);

    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    cJSON_free(json_str);
    cJSON_Delete(response);

    ESP_LOGI(TAG, "Group control completed: %d success, %d failed", successCount, failCount);

    return ESP_OK;
}

/**
 * @brief Registers all API routes with their handler functions.
 */
static void registerRoutes(httpd_handle_t server, ServerContext* ctx) {
    // GET /lights/all
    httpd_uri_t get_all_lights = {
        .uri       = "/lights/all",
        .method    = HTTP_GET,
        .handler   = handleGetAllLights,
        .user_ctx  = (void*)ctx
    };
    httpd_register_uri_handler(server, &get_all_lights);

    // GET /lights/group
    httpd_uri_t get_light_groups = {
        .uri       = "/lights/group",
        .method    = HTTP_GET,
        .handler   = handleGetLightGroups,
        .user_ctx  = (void*)ctx
    };
    httpd_register_uri_handler(server, &get_light_groups);

    // PUT /lights - control lights by group
    httpd_uri_t put_lights = {
        .uri       = "/lights",
        .method    = HTTP_PUT,
        .handler   = handleControlLightGroup,
        .user_ctx  = (void*)ctx
    };
    httpd_register_uri_handler(server, &put_lights);

    // PUT /lights/group
    httpd_uri_t set_light_group = {
        .uri       = "/lights/group",
        .method    = HTTP_PUT,
        .handler   = handleSetLightGroup,
        .user_ctx  = (void*)ctx
    };
    httpd_register_uri_handler(server, &set_light_group);

    ESP_LOGI(TAG, "Registered 8 routes");
}

/**
 * @brief Background task to update cached device JSON periodically.
 */
void update_device_cache_task(void* pvParameters) {
    auto* device_map = static_cast<const std::map<std::string, DeviceInfo>*>(pvParameters);

    ESP_LOGI(TAG, "Device cache update task started");

    while (1) {
        std::string new_json = device_map_to_json(device_map);

        if (xSemaphoreTake(server_cache->mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            server_cache->cached_devices_json = new_json;
            xSemaphoreGive(server_cache->mutex);
            ESP_LOGD(TAG, "Updated device cache (%d bytes)", new_json.length());
        }

        vTaskDelay(pdMS_TO_TICKS(2000)); // Update every 2 seconds
    }
}

/**
 * @brief Starts the HTTP server on port 80.
 */
httpd_handle_t http_server_start(const std::map<std::string, DeviceInfo>* device_map, const std::map<std::string, DeviceInfo>* device_serial_map, LightGroupCache* light_group_cache) {
    ESP_LOGI(TAG, "Starting HTTP server...");

    // Initialize cache
    server_cache = new ServerCache();

    // Create server context
    static ServerContext ctx;
    ctx.device_map = device_map;
    ctx.device_serial_map = device_serial_map;
    ctx.light_group_cache = light_group_cache;

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.task_priority = 1;
    config.stack_size = 12288;  // Increased from 8192 to handle larger JSON payloads and string operations
    config.core_id = 0;
    config.server_port = 80;
    config.max_open_sockets = 4;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s (0x%x)", esp_err_to_name(err), err);
        return NULL;
    }

    ESP_LOGI(TAG, "HTTP server started successfully");

    registerRoutes(server, &ctx);

    // Start background task to update cache
    xTaskCreatePinnedToCore(
        update_device_cache_task,
        "device_cache_updater",
        4096,
        (void*)device_map,
        2,  // Lower priority than HTTP server
        NULL,
        0
    );

    ESP_LOGI(TAG, "HTTP server listening on port 80");
    return server;
}
