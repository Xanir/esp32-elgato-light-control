#include "nvs_helper.h"
#include "nvs.h"
#include "nvs_flash.h"

// Define the application NVS namespace
const std::string APP_NVS_NS = "elights";

/**
 * @brief Initializes the NVS flash storage and handles potential errors.
 * * This must be called once at startup before any NVS read/write operations.
 * @return esp_err_t The result of the initialization (ESP_OK on success).
 */
esp_err_t initialize_nvs() {
    esp_err_t ret = nvs_flash_init();
    
    // Check if the partition is corrupted or not formatted
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); // Erase the partition
        ret = nvs_flash_init();            // Re-initialize
    }
    ESP_ERROR_CHECK(ret);
    return ret;
}

bool set_nvs_string_value(const std::string& nvs_namespace, const std::string& key, const std::string& value) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(nvs_namespace.c_str(), NVS_READWRITE, &nvs_handle);

    if (err != ESP_OK) {
        return false;
    }

    err = nvs_set_str(nvs_handle, key.c_str(), value.c_str());

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return (err == ESP_OK);
}

/**
 * @brief Reads a string value from the NVS "wifi" namespace using a dynamic key.
 * * * This function performs a two-step NVS read to handle dynamic string sizes:
 * * 1. Read the required size (by passing NULL buffer).
 * * 2. Allocate memory and read the actual string value.
 *
 * @param key The key (item name) to look up in the NVS namespace (e.g., "ssid").
 * @return std::string The string value read from NVS, or an empty string on failure.
 */
std::string get_nvs_string_value(const std::string& key) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    std::string result = ""; // Default return value on failure
    size_t required_size = 0;

    // --- 1. Open the NVS Namespace (assuming "wifi") ---
    err = nvs_open(APP_NVS_NS.c_str(), NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return result; 
    }

    // --- 2. First call: get the required size (pass NULL buffer) ---
    err = nvs_get_str(nvs_handle, key.c_str(), NULL, &required_size);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return result;
    }

    // --- 3. Allocate buffer and Second call: read the string value ---
    // required_size includes the null terminator.
    char* buffer = (char*)malloc(required_size);
    if (buffer == NULL) {
        nvs_close(nvs_handle);
        return result;
    }

    err = nvs_get_str(nvs_handle, key.c_str(), buffer, &required_size);
    
    if (err == ESP_OK) {
        result = buffer; // Convert C-string to std::string
    }

    // --- 4. Cleanup ---
    free(buffer);
    nvs_close(nvs_handle);
    return result;
}
