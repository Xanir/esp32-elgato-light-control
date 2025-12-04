#include <string>
#include <map>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

extern "C" {
    #include <cJSON.h>
}

#include "http_server.h"
#include "http_requester.h"

static const char* TAG = "HTTP_SERVER";

/**
 * @brief Converts the device map to a JSON string.
 */
static std::string device_map_to_json(const std::map<std::string, DeviceInfo>& device_map) {
    cJSON *root = cJSON_CreateArray();
    
    for (const auto& pair : device_map) {
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

/**
 * @brief Handles a single HTTP client connection.
 */
static void handle_http_request(int client_sock, std::map<std::string, DeviceInfo>* device_map) {
    const int BUFSIZE = 1024;
    char buffer[BUFSIZE];
    
    // Read the HTTP request
    int bytes_received = recv(client_sock, buffer, BUFSIZE - 1, 0);
    if (bytes_received <= 0) {
        close(client_sock);
        return;
    }
    
    buffer[bytes_received] = '\0';
    std::string request(buffer);
    
    // Parse the request line (e.g., "GET /lights/all HTTP/1.1")
    size_t method_end = request.find(' ');
    size_t path_end = request.find(' ', method_end + 1);
    
    if (method_end == std::string::npos || path_end == std::string::npos) {
        close(client_sock);
        return;
    }
    
    std::string method = request.substr(0, method_end);
    std::string path = request.substr(method_end + 1, path_end - method_end - 1);
    
    std::string response;
    
    if (method == "GET" && path == "/lights/all") {
        // Generate JSON response
        std::string json_body = device_map_to_json(*device_map);
        
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
        response += std::to_string(json_body.length());
        response += "\r\nConnection: close\r\n\r\n";
        response += json_body;
    } else {
        // 404 Not Found
        const char* body = "{\"error\":\"Not Found\"}";
        size_t body_len = 23;
        response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: ";
        response += std::to_string(body_len);
        response += "\r\nConnection: close\r\n\r\n";
        response += body;
    }
    
    // Send response
    send(client_sock, response.c_str(), response.length(), 0);
    close(client_sock);
}

/**
 * @brief Task function to handle HTTP server requests.
 */
void http_server_task(void* pvParameters) {
    HttpServerConfig* config = (HttpServerConfig*)pvParameters;
    int server_sock = config->server_socket;
    std::map<std::string, DeviceInfo>* device_map = config->device_map;
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_addr_len);
        
        if (client_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        handle_http_request(client_sock, device_map);
    }
}

/**
 * @brief Starts the HTTP server on port 80.
 */
int http_server_start(std::map<std::string, DeviceInfo>* device_map) {
    ESP_LOGI(TAG, "Starting HTTP server...");
    
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        return -1;
    }
    ESP_LOGI(TAG, "Socket created successfully");
    
    // Set socket option to reuse address
    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        ESP_LOGW(TAG, "Failed to set SO_REUSEADDR: errno %d", errno);
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(80);
    
    ESP_LOGI(TAG, "Attempting to bind to port 80...");
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: errno %d", errno);
        close(server_sock);
        return -1;
    }
    ESP_LOGI(TAG, "Bind successful");
    
    ESP_LOGI(TAG, "Starting listen...");
    if (listen(server_sock, 5) < 0) {
        ESP_LOGE(TAG, "Listen failed: errno %d", errno);
        close(server_sock);
        return -1;
    }
    ESP_LOGI(TAG, "Listen successful");
    
    // Create the server task
    HttpServerConfig* config = new HttpServerConfig();
    config->server_socket = server_sock;
    config->device_map = device_map;
    
    ESP_LOGI(TAG, "Creating HTTP server task...");
    BaseType_t task_result = xTaskCreatePinnedToCore(http_server_task, "http_srv", 4096, config, 1, NULL, 0);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create HTTP server task");
        close(server_sock);
        delete config;
        return -1;
    }
    
    ESP_LOGI(TAG, "HTTP server started successfully on port 80");
    return server_sock;
}
