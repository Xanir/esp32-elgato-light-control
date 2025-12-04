#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <string>
#include <sstream>

/**
 * @brief Structure to hold the parsed device information from the JSON response.
 */
struct DeviceInfo {
    std::string ip;
    std::string productName;
    int hardwareBoardType = 0;
    std::string hardwareRevision;
    std::string macAddress;
    int firmwareBuildNumber = 0;
    std::string firmwareVersion;
    std::string serialNumber;
    std::string displayName;
    std::string error; // To store error message if parsing/request fails

    /**
     * @brief Provides a formatted string representation of the parsed data.
     * Defined in http_client.cpp.
     */
    std::string toString() const;
};

// --- Main HTTP Client Function Declaration ---

/**
 * @brief Sends an HTTP GET request to a specified host, port, and path, and parses the JSON response body.
 *
 * @param host The hostname or IP address.
 * @param port The port number (usually 80 for HTTP).
 * @param path The path of the resource.
 * @return A DeviceInfo struct containing the parsed data, or an error message in the 'error' field.
 */
DeviceInfo sendHttpGetRequest(const std::string& host, int port, const std::string& path);

#endif // HTTP_CLIENT_H