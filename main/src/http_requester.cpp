#include <string>
#include <cstring>
#include <errno.h>
#include <sstream>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"

extern "C" {
    #include <cJSON.h>
}

#include "http_requester.h"

// --- Data Structure for Parsed Response ---

static const char* TAG = "HTTP_REQUESTER";

std::string DeviceInfo::toString() const {
    if (!error.empty()) {
        return "Error: " + error;
    }

    std::stringstream ss;
    ss << "--- Device Information ---\n"
       << "  IP Address:          " << ip << "\n"
       << "  Product Name:        " << productName << "\n"
       << "  HW Board Type:       " << hardwareBoardType << "\n"
       << "  HW Revision:         " << hardwareRevision << "\n"
       << "  MAC Address:         " << macAddress << "\n"
       << "  FW Build Number:     " << firmwareBuildNumber << "\n"
       << "  FW Version:          " << firmwareVersion << "\n"
       << "  Serial Number:       " << serialNumber << "\n"
       << "  Display Name:        " << displayName << "\n"
       << "--------------------------";
    return ss.str();
}

// --- Manual JSON Parsing Helpers (Simplified for demonstration) ---

/**
 * @brief Parses the JSON body string into the DeviceInfo struct.
 */
DeviceInfo parseJsonBody(const std::string &json_body) {
    DeviceInfo info;
    cJSON *root = cJSON_Parse(json_body.c_str());
    if (!root) {
        info.error = "Failed to parse JSON body.";
        return info;
    }

    // Helper lambda for string extraction
    auto getString = [root](const char* key) -> std::string {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsString(item) && (item->valuestring != nullptr)) {
            return std::string(item->valuestring);
        }
        return "";
    };

    // Helper lambda for int extraction
    auto getInt = [root](const char* key) -> int {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsNumber(item)) {
            return item->valueint;
        }
        return 0;
    };

    info.productName = getString("productName");
    info.hardwareBoardType = getInt("hardwareBoardType");
    info.hardwareRevision = getString("hardwareRevision");
    info.macAddress = getString("macAddress");
    info.firmwareBuildNumber = getInt("firmwareBuildNumber");
    info.firmwareVersion = getString("firmwareVersion");
    info.serialNumber = getString("serialNumber");
    info.displayName = getString("displayName");

    cJSON_Delete(root);
    return info;
}

/**
 * @brief Sends an HTTP PUT request with JSON body to a specified host, port, and path.
 */
std::string sendHttpPutRequest(const std::string &host, const int &port, const std::string &path, const std::string &json_body) {
    int64_t start_time = esp_timer_get_time();

    // Construct full URL
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d%s", host.c_str(), port, path.c_str());

    // Configure HTTP client
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 2000;
    config.keep_alive_enable = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return "";
    }

    // Set PUT method and headers
    esp_http_client_set_method(client, HTTP_METHOD_PUT);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Open connection and write request body
    esp_err_t err = esp_http_client_open(client, json_body.length());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return "";
    }

    // Write request body
    int wlen = esp_http_client_write(client, json_body.c_str(), json_body.length());
    if (wlen < 0) {
        ESP_LOGE(TAG, "Failed to write request body");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return "";
    }

    // Fetch response headers
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    std::string response;
    if (status >= 200 && status < 300) {
        // Read response data
        char buffer[1024];
        int read_len;

        while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[read_len] = '\0';
            response.append(buffer, read_len);
        }

        int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;
        ESP_LOGI(TAG, "PUT request to %s completed in %lld ms (status=%d)", 
                 url, elapsed_ms, status);
    } else {
        ESP_LOGE(TAG, "PUT request failed with HTTP %d", status);
    }

    esp_http_client_close(client);

    esp_http_client_cleanup(client);
    return response;
}

// --- Elgato API Functions ---

/**
 * @brief Parses Elgato lights JSON response.
 */
ElgatoLight parseElgatoLightsResponse(const std::string &json_body) {
    ElgatoLight light;
    cJSON *root = cJSON_Parse(json_body.c_str());
    if (!root) {
        light.error = "Failed to parse JSON response";
        return light;
    }

    cJSON *lights_array = cJSON_GetObjectItemCaseSensitive(root, "lights");
    if (!cJSON_IsArray(lights_array) || cJSON_GetArraySize(lights_array) == 0) {
        light.error = "No lights found in response";
        cJSON_Delete(root);
        return light;
    }

    cJSON *first_light = cJSON_GetArrayItem(lights_array, 0);

    cJSON *on = cJSON_GetObjectItemCaseSensitive(first_light, "on");
    if (cJSON_IsNumber(on)) {
        light.on = on->valueint;
    }

    cJSON *brightness = cJSON_GetObjectItemCaseSensitive(first_light, "brightness");
    if (cJSON_IsNumber(brightness)) {
        light.brightness = brightness->valueint;
    }

    cJSON *temperature = cJSON_GetObjectItemCaseSensitive(first_light, "temperature");
    if (cJSON_IsNumber(temperature)) {
        light.temperature = temperature->valueint;
    }

    cJSON_Delete(root);
    return light;
}

ElgatoLight setLight(const std::string &ip, int brightness, std::optional<int> temperature) {
    ElgatoLight light;

    // Validate parameters
    if (brightness < 0 || brightness > 100) {
        light.error = "Brightness must be between 0 and 100";
        ESP_LOGE(TAG, "%s", light.error.c_str());
        return light;
    }

    if (temperature.has_value() && (temperature.value() < 143 || temperature.value() > 344)) {
        light.error = "Temperature must be between 143 and 344";
        ESP_LOGE(TAG, "%s", light.error.c_str());
        return light;
    }

    // Create JSON body
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "numberOfLights", 1);

    cJSON *lights_array = cJSON_CreateArray();
    cJSON *light_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(light_obj, "on", brightness > 0 ? 1 : 0);
    cJSON_AddNumberToObject(light_obj, "brightness", brightness);

    // Only add temperature if provided
    if (temperature.has_value()) {
        cJSON_AddNumberToObject(light_obj, "temperature", temperature.value());
    }

    cJSON_AddItemToArray(lights_array, light_obj);
    cJSON_AddItemToObject(root, "lights", lights_array);

    char *json_string = cJSON_PrintUnformatted(root);
    std::string json_body(json_string);
    cJSON_free(json_string);
    cJSON_Delete(root);

    // Send PUT request
    std::string response = sendHttpPutRequest(ip, 9123, "/elgato/lights", json_body);

    if (response.empty()) {
        light.error = "Failed request: Update to " + ip;
        ESP_LOGE(TAG, "%s", light.error.c_str());
        return light;
    }

    // Parse response
    return parseElgatoLightsResponse(response);
}

ElgatoLight getLight(const std::string &ip) {
    ElgatoLight light;

    // Construct URL
    char url[256];
    snprintf(url, sizeof(url), "http://%s:9123/elgato/lights", ip.c_str());

    // Configure HTTP client
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 2000;
    config.keep_alive_enable = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        light.error = "Failed request: Getting light info for " + ip;
        ESP_LOGE(TAG, "%s", light.error.c_str());
        return light;
    }

    // Open connection
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        light.error = "Failed to open: " + std::string(esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", light.error.c_str());
        esp_http_client_cleanup(client);
        return light;
    }

    // Fetch headers
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status >= 200 && status < 300) {
        // Read response data
        char buffer[1024];
        int read_len;
        std::string json_body;

        while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[read_len] = '\0';
            json_body.append(buffer, read_len);
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (!json_body.empty()) {
            return parseElgatoLightsResponse(json_body);
        }
    }

    light.error = "HTTP " + std::to_string(status);
    ESP_LOGE(TAG, "GET light failed with status %d", status);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return light;
}

DeviceInfo getInfo(const std::string &ip) {
    DeviceInfo info = sendHttpGetRequest(ip, 9123, "/elgato/accessory-info");
    if (!info.error.empty()) {
        info.error = "Failed request: Getting accessory info for " + ip;
        ESP_LOGE(TAG, "%s", info.error.c_str());
    }
    return info;
}

bool setDeviceName(const std::string &ip, const std::string &name) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "displayName", name.c_str());

    char *json_string = cJSON_PrintUnformatted(root);
    std::string json_body(json_string);
    cJSON_free(json_string);
    cJSON_Delete(root);

    std::string response = sendHttpPutRequest(ip, 9123, "/elgato/accessory-info", json_body);

    if (response.empty()) {
        ESP_LOGE(TAG, "Failed request: Setting device name for %s", ip.c_str());
        return false;
    }

    return true;
}

// --- Main HTTP Client Function ---

/**
 * @brief Sends an HTTP GET request to a specified host, port, and path, and parses the JSON response body.
 *
 * @param host The hostname or IP address.
 * @param port The port number (usually 80 for HTTP).
 * @param path The path of the resource.
 * @return A DeviceInfo struct containing the parsed data, or an error message in the 'error' field.
 */
DeviceInfo sendHttpGetRequest(const std::string &host, const int &port, const std::string &path) {
    DeviceInfo error_result;

    // Construct URL
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d%s", host.c_str(), port, path.c_str());

    // Configure HTTP client
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 2000;
    config.keep_alive_enable = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        error_result.error = "Failed to initialize HTTP client";
        ESP_LOGE(TAG, "%s", error_result.error.c_str());
        return error_result;
    }

    // Open connection and fetch headers
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        error_result.error = "Failed to open connection: " + std::string(esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", error_result.error.c_str());
        esp_http_client_cleanup(client);
        return error_result;
    }

    // Fetch headers
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "GET %s -> HTTP %d (Content-Length: %d)", url, status, content_length);

    if (status >= 200 && status < 300) {
        // Read response body
        char buffer[2048];
        int total_read = 0;
        int read_len;
        std::string json_body;

        while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[read_len] = '\0';
            json_body.append(buffer, read_len);
            total_read += read_len;
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (!json_body.empty()) {
            ESP_LOGI(TAG, "Successfully read %d bytes", total_read);

            // Parse the JSON body
            DeviceInfo info = parseJsonBody(json_body);
            info.ip = host;
            return info;
        } else {
            error_result.error = "Empty response body";
            ESP_LOGE(TAG, "%s", error_result.error.c_str());
        }
    } else {
        error_result.error = "HTTP status " + std::to_string(status);
        ESP_LOGE(TAG, "Bad HTTP status: %d", status);
        esp_http_client_close(client);
    }

    esp_http_client_cleanup(client);
    return error_result;
}
