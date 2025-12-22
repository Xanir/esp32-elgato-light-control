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
 * @brief Handler for GET /elgato/lights - gets light state from specific device.
 * Requires query parameter: ip=<device_ip>
 */
static esp_err_t handleGetLight(httpd_req_t *req) {
    char query_buf[256];
    char ip[32];

    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing query parameters\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (httpd_query_key_value(query_buf, "ip", ip, sizeof(ip)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing 'ip' parameter\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ElgatoLight light = getLight(std::string(ip));

    if (!light.error.empty()) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", light.error.c_str());
        char *json_str = cJSON_PrintUnformatted(error);

        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));

        cJSON_free(json_str);
        cJSON_Delete(error);
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "on", light.on);
    cJSON_AddNumberToObject(root, "brightness", light.brightness);
    cJSON_AddNumberToObject(root, "temperature", light.temperature);

    char *json_str = cJSON_PrintUnformatted(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    cJSON_free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

/**
 * @brief Handler for PUT /elgato/lights - sets light state for specific device.
 * Requires query parameters: ip=<device_ip>, brightness=<0-100>, temperature=<143-344>
 */
static esp_err_t handleSetLight(httpd_req_t *req) {
    char query_buf[256];
    char ip[32], brightness_str[8], temperature_str[8];

    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing required parameters: ip, brightness, temperature\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (httpd_query_key_value(query_buf, "ip", ip, sizeof(ip)) != ESP_OK ||
        httpd_query_key_value(query_buf, "brightness", brightness_str, sizeof(brightness_str)) != ESP_OK ||
        httpd_query_key_value(query_buf, "temperature", temperature_str, sizeof(temperature_str)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing required parameters: ip, brightness, temperature\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int brightness = atoi(brightness_str);
    int temperature = atoi(temperature_str);

    ElgatoLight light = setLight(std::string(ip), brightness, temperature);

    if (!light.error.empty()) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", light.error.c_str());
        char *json_str = cJSON_PrintUnformatted(error);
        
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        
        cJSON_free(json_str);
        cJSON_Delete(error);
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "on", light.on);
    cJSON_AddNumberToObject(root, "brightness", light.brightness);
    cJSON_AddNumberToObject(root, "temperature", light.temperature);

    char *json_str = cJSON_PrintUnformatted(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    cJSON_free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

/**
 * @brief Handler for GET /elgato/accessory-info - gets device info.
 * Requires query parameter: ip=<device_ip>
 */
static esp_err_t handleGetInfo(httpd_req_t *req) {
    char query_buf[256];
    char ip[32];

    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing query parameters\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (httpd_query_key_value(query_buf, "ip", ip, sizeof(ip)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing 'ip' parameter\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    DeviceInfo info = getInfo(std::string(ip));

    if (!info.error.empty()) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", info.error.c_str());
        char *json_str = cJSON_PrintUnformatted(error);

        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));

        cJSON_free(json_str);
        cJSON_Delete(error);
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "productName", info.productName.c_str());
    cJSON_AddNumberToObject(root, "hardwareBoardType", info.hardwareBoardType);
    cJSON_AddStringToObject(root, "firmwareVersion", info.firmwareVersion.c_str());
    cJSON_AddNumberToObject(root, "firmwareBuildNumber", info.firmwareBuildNumber);
    cJSON_AddStringToObject(root, "serialNumber", info.serialNumber.c_str());
    cJSON_AddStringToObject(root, "displayName", info.displayName.c_str());

    char *json_str = cJSON_PrintUnformatted(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    cJSON_free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

/**
 * @brief Handler for PUT /elgato/accessory-info - sets device name.
 * Requires query parameters: ip=<device_ip>, name=<new_name>
 */
static esp_err_t handleSetDeviceName(httpd_req_t *req) {
    char query_buf[256];
    char ip[32], name[128];

    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing required parameters: ip, name\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (httpd_query_key_value(query_buf, "ip", ip, sizeof(ip)) != ESP_OK ||
        httpd_query_key_value(query_buf, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing required parameters: ip, name\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    bool success = setDeviceName(std::string(ip), std::string(name));

    if (!success) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Failed to set device name\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

/**
 * @brief Registers all API routes with their handler functions.
 */
static void registerRoutes(httpd_handle_t server, const std::map<std::string, DeviceInfo>* device_map) {
    // GET /lights/all
    httpd_uri_t get_all_lights = {
        .uri       = "/lights/all",
        .method    = HTTP_GET,
        .handler   = handleGetAllLights,
        .user_ctx  = (void*)device_map
    };
    httpd_register_uri_handler(server, &get_all_lights);

    // GET /elgato/lights
    httpd_uri_t get_light = {
        .uri       = "/elgato/lights",
        .method    = HTTP_GET,
        .handler   = handleGetLight,
        .user_ctx  = (void*)device_map
    };
    httpd_register_uri_handler(server, &get_light);

    // PUT /elgato/lights
    httpd_uri_t set_light = {
        .uri       = "/elgato/lights",
        .method    = HTTP_PUT,
        .handler   = handleSetLight,
        .user_ctx  = (void*)device_map
    };
    httpd_register_uri_handler(server, &set_light);

    // GET /elgato/accessory-info
    httpd_uri_t get_info = {
        .uri       = "/elgato/accessory-info",
        .method    = HTTP_GET,
        .handler   = handleGetInfo,
        .user_ctx  = (void*)device_map
    };
    httpd_register_uri_handler(server, &get_info);

    // PUT /elgato/accessory-info
    httpd_uri_t set_device_name = {
        .uri       = "/elgato/accessory-info",
        .method    = HTTP_PUT,
        .handler   = handleSetDeviceName,
        .user_ctx  = (void*)device_map
    };
    httpd_register_uri_handler(server, &set_device_name);

    ESP_LOGI(TAG, "Registered 5 routes");
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
httpd_handle_t http_server_start(const std::map<std::string, DeviceInfo>* device_map) {
    ESP_LOGI(TAG, "Starting HTTP server...");

    // Initialize cache
    server_cache = new ServerCache();

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.task_priority = 1;
    config.stack_size = 8192;
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

    registerRoutes(server, device_map);

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
