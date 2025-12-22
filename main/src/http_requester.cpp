#include <string>
#include <cstring>
#include <errno.h>
#include <sstream>

// POSIX socket headers (for Linux/macOS. For Windows, you'd use Winsock2.h)
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>

extern "C" {
    #include <cJSON.h>
}

#include "esp_log.h"
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
    struct addrinfo hints, *server_info, *p;
    int sockfd = -1;

    // 1. Resolve hostname/IP and prepare connection info
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);

    int rv = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &server_info);
    if (rv != 0) {
        ESP_LOGE(TAG, "Host resolution failed for PUT request");
        return "";
    }

    // Loop through all results and connect
    for(p = server_info; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            continue;
        }
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            continue;
        }
        break;
    }
    freeaddrinfo(server_info);

    if (p == NULL) {
        ESP_LOGE(TAG, "Failed to connect to host for PUT request");
        return "";
    }

    // Set socket receive timeout
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // 2. Construct and send the HTTP PUT request
    std::stringstream request_stream;
    request_stream << "PUT " << path << " HTTP/1.1\r\n";
    request_stream << "Host: " << host << "\r\n";
    request_stream << "Content-Type: application/json\r\n";
    request_stream << "Content-Length: " << json_body.length() << "\r\n";
    request_stream << "Connection: close\r\n";
    request_stream << "\r\n";
    request_stream << json_body;

    std::string request = request_stream.str();

    if (send(sockfd, request.c_str(), request.length(), 0) == -1) {
        close(sockfd);
        ESP_LOGE(TAG, "Failed to send PUT request");
        return "";
    }

    // 3. Receive the response
    std::string raw_response;
    const int BUFSIZE = 1024;
    char buffer[BUFSIZE];  // Local buffer, not static
    int bytes_received;

    while ((bytes_received = recv(sockfd, buffer, BUFSIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        raw_response.append(buffer, bytes_received);
    }

    close(sockfd);

    // 4. Separate headers from body
    size_t body_start = raw_response.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        ESP_LOGE(TAG, "Invalid HTTP response format in PUT request");
        return "";
    }

    return raw_response.substr(body_start + 4);
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

ElgatoLight setLight(const std::string &ip, int brightness, int temperature) {
    ElgatoLight light;

    // Validate parameters
    if (brightness < 0 || brightness > 100) {
        light.error = "Brightness must be between 0 and 100";
        ESP_LOGE(TAG, "%s", light.error.c_str());
        return light;
    }

    if (temperature < 143 || temperature > 344) {
        light.error = "Temperature must be between 143 and 344";
        ESP_LOGE(TAG, "%s", light.error.c_str());
        return light;
    }

    // Create JSON body
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "numberOfLights", 1);

    cJSON *lights_array = cJSON_CreateArray();
    cJSON *light_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(light_obj, "on", 1);
    cJSON_AddNumberToObject(light_obj, "brightness", brightness);
    cJSON_AddNumberToObject(light_obj, "temperature", temperature);
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

    // Send GET request using existing function but parse differently
    struct addrinfo hints, *server_info, *p;
    int sockfd = -1;

    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = "9123";

    int rv = getaddrinfo(ip.c_str(), port_str.c_str(), &hints, &server_info);
    if (rv != 0) {
        light.error = "Failed request: Getting light info for " + ip;
        ESP_LOGE(TAG, "%s", light.error.c_str());
        return light;
    }

    for(p = server_info; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            continue;
        }
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            continue;
        }
        break;
    }
    freeaddrinfo(server_info);

    if (p == NULL) {
        light.error = "Failed request: Getting light info for " + ip;
        ESP_LOGE(TAG, "%s", light.error.c_str());
        return light;
    }

    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::stringstream request_stream;
    request_stream << "GET /elgato/lights HTTP/1.1\r\n";
    request_stream << "Host: " << ip << "\r\n";
    request_stream << "Connection: close\r\n\r\n";

    std::string request = request_stream.str();

    if (send(sockfd, request.c_str(), request.length(), 0) == -1) {
        close(sockfd);
        light.error = "Failed request: Getting light info for " + ip;
        ESP_LOGE(TAG, "%s", light.error.c_str());
        return light;
    }

    std::string raw_response;
    const int BUFSIZE = 1024;
    char buffer[BUFSIZE];
    int bytes_received;

    while ((bytes_received = recv(sockfd, buffer, BUFSIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        raw_response.append(buffer, bytes_received);
    }

    close(sockfd);

    size_t body_start = raw_response.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        light.error = "Invalid HTTP response format";
        ESP_LOGE(TAG, "%s", light.error.c_str());
        return light;
    }

    std::string json_body = raw_response.substr(body_start + 4);
    return parseElgatoLightsResponse(json_body);
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
    struct addrinfo hints, *server_info, *p;
    int sockfd = -1;

    // 1. Resolve hostname/IP and prepare connection info
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;    // TCP stream sockets

    std::string port_str = std::to_string(port);

    int rv = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &server_info);
    if (rv != 0) {
        // Avoid using gai_strerror (may not be available on this toolchain).
        error_result.error = "Host resolution failed: error code " + std::to_string(rv);
        return error_result;
    }

    // Loop through all results and connect
    int last_socket_errno = 0;
    int last_connect_errno = 0;
    for(p = server_info; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            last_socket_errno = errno;
            ESP_LOGW(TAG, "Socket creation failed: errno %d (%s)", errno, strerror(errno));
            continue; 
        }
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            last_connect_errno = errno;
            ESP_LOGW(TAG, "Connection failed to %s:%d - errno %d (%s)", host.c_str(), port, errno, strerror(errno));
            close(sockfd);
            continue; 
        }
        break; // Success
    }
    freeaddrinfo(server_info); 

    if (p == NULL) {
        error_result.error = "Failed to connect to host. Socket errno: " + std::to_string(last_socket_errno) + 
                            ", Connect errno: " + std::to_string(last_connect_errno);
        return error_result;
    }

    // Set socket receive timeout to prevent hanging
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // 2. Construct and send the HTTP GET request
    std::stringstream request_stream;
    request_stream << "GET " << path << " HTTP/1.1\r\n";
    request_stream << "Host: " << host << "\r\n";
    request_stream << "User-Agent: CppHttpClient/1.0\r\n";
    request_stream << "Connection: close\r\n"; 
    request_stream << "\r\n"; 

    std::string request = request_stream.str();

    if (send(sockfd, request.c_str(), request.length(), 0) == -1) {
        close(sockfd);
        error_result.error = "Failed to send request: " + std::string(strerror(errno));
        return error_result;
    }

    // 3. Receive the raw response
    std::string raw_response;
    const int BUFSIZE = 1024;
    char buffer[BUFSIZE];  // Local buffer, not static
    int bytes_received;

    while ((bytes_received = recv(sockfd, buffer, BUFSIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        raw_response.append(buffer, bytes_received);
    }

    if (bytes_received == -1) {
        int err = errno;
        close(sockfd);
        if ((err == EAGAIN || err == EWOULDBLOCK) && !raw_response.empty()) {
            // Timeout but got data, continue
        } else {
            error_result.error = "Recv fail";
            return error_result;
        }
    }

    close(sockfd); 

    // 4. Separate headers from body (Body starts after the first \r\n\r\n)
    size_t body_start = raw_response.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        error_result.error = "Invalid HTTP response format (no end of headers).";
        return error_result;
    }

    std::string json_body = raw_response.substr(body_start + 4);

    // 5. Parse the JSON body
    DeviceInfo info = parseJsonBody(json_body);
    info.ip = host; // Set the IP address field

    return info;
}
