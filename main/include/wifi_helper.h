#pragma once

#include <string>
#include "esp_netif.h"

/**
 * @brief Initializes WiFi in station mode and connects to the specified network.
 * This function will block until the connection succeeds or fails after maximum retries.
 * @param wifi_ssid The SSID of the WiFi network to connect to
 * @param wifi_password The password for the WiFi network
 * @return esp_netif_t* Pointer to the network interface, or NULL on failure
 */
esp_netif_t* wifi_init_station(const std::string& wifi_ssid, const std::string& wifi_password);

/**
 * @brief Gets the current WiFi IP address as a string.
 * @return const char* The IP address string, or "0.0.0.0" if not connected
 */
const char* get_wifi_ip();
