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
 */
static esp_err_t handleGetAllLights(httpd_req_t *req) {
    auto* device_map = static_cast<std::map<std::string, DeviceInfo>*>(req->user_ctx);

    std::string json = device_map_to_json(device_map);

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
 * @brief Starts the HTTP server on port 80.
 */
httpd_handle_t http_server_start(const std::map<std::string, DeviceInfo>* device_map) {
    ESP_LOGI(TAG, "Starting HTTP server...");

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.task_priority = 1;
    config.stack_size = 4096;
    config.core_id = 0;
    config.server_port = 80;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    ESP_LOGI(TAG, "HTTP server started successfully");

    registerRoutes(server, device_map);

    ESP_LOGI(TAG, "HTTP server listening on port 80");
    return server;
}
