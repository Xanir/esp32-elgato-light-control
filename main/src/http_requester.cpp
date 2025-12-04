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
DeviceInfo parseJsonBody(const std::string& json_body) {
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


// --- Main HTTP Client Function ---

/**
 * @brief Sends an HTTP GET request to a specified host, port, and path, and parses the JSON response body.
 *
 * @param host The hostname or IP address.
 * @param port The port number (usually 80 for HTTP).
 * @param path The path of the resource.
 * @return A DeviceInfo struct containing the parsed data, or an error message in the 'error' field.
 */
DeviceInfo sendHttpGetRequest(const std::string& host, int port, const std::string& path) {
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
    for(p = server_info; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            continue; 
        }
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            continue; 
        }
        break; // Success
    }
    freeaddrinfo(server_info); 

    if (p == NULL) {
        error_result.error = "Failed to connect to host.";
        return error_result;
    }

    // Set socket receive timeout to prevent hanging
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

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
    static char buffer[BUFSIZE];
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
