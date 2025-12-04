#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <string>
#include <map>
#include "http_requester.h"

/**
 * @brief Starts the HTTP server on port 80.
 * 
 * @param device_map Pointer to the map of device serial numbers to DeviceInfo structs.
 * @return int Socket file descriptor on success, -1 on failure.
 */
int http_server_start(std::map<std::string, DeviceInfo>* device_map);

/**
 * @brief Task function to handle HTTP server requests.
 * Should be run as a FreeRTOS task.
 * 
 * @param pvParameters Pointer to HTTP server configuration.
 */
void http_server_task(void* pvParameters);

/**
 * @brief Configuration structure for HTTP server task.
 */
struct HttpServerConfig {
    int server_socket;
    std::map<std::string, DeviceInfo>* device_map;
};

#endif // HTTP_SERVER_H
