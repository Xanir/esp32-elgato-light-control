#pragma once

#include <string>
#include "esp_err.h"

// NVS namespace used by the application
extern const std::string APP_NVS_NS;

/**
 * @brief Initializes the NVS flash storage and handles potential errors.
 * This must be called once at startup before any NVS read/write operations.
 * @return esp_err_t The result of the initialization (ESP_OK on success).
 */
esp_err_t initialize_nvs();

/**
 * @brief Writes a string value to NVS.
 * @param nvs_namespace The NVS namespace to use (e.g., "elights")
 * @param key The key name to store the value under
 * @param value The string value to store
 * @return bool True if successful, false otherwise
 */
bool set_nvs_string_value(const std::string& nvs_namespace, const std::string& key, const std::string& value);

/**
 * @brief Reads a string value from NVS using the application's namespace.
 * This function performs a two-step NVS read to handle dynamic string sizes:
 * 1. Read the required size (by passing NULL buffer).
 * 2. Allocate memory and read the actual string value.
 * @param key The key (item name) to look up in the NVS namespace (e.g., "WIFI_SSID")
 * @return std::string The string value read from NVS, or an empty string on failure
 */
std::string get_nvs_string_value(const std::string& key);
