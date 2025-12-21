#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <string>
#include <map>
#include "http_requester.h"

extern "C" {
    #include "esp_http_server.h"
}

/**
 * @brief Starts the HTTP server on port 80.
 * 
 * @param device_map Pointer to the map of device serial numbers to DeviceInfo structs.
 * @return httpd_handle_t Server handle on success, NULL on failure.
 */
httpd_handle_t http_server_start(const std::map<std::string, DeviceInfo>* device_map);

#endif // HTTP_SERVER_H
