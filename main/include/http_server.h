#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <string>
#include <map>
#include "http_requester.h"
#include "cache_lights.h"

extern "C" {
    #include "esp_http_server.h"
}

/**
 * @brief Starts the HTTP server on port 80.
 * 
 * @param device_map Pointer to the map of device IP addresses to DeviceInfo structs.
 * @param device_serial_map Pointer to the map of device serial numbers to DeviceInfo structs.
 * @param light_group_cache Pointer to the LightGroupCache instance.
 * @return httpd_handle_t Server handle on success, NULL on failure.
 */
httpd_handle_t http_server_start(const std::map<std::string, DeviceInfo>* device_map, const std::map<std::string, DeviceInfo>* device_serial_map, LightGroupCache* light_group_cache);

#endif // HTTP_SERVER_H
