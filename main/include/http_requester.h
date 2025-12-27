#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <string>
#include <sstream>
#include <optional>

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

/**
 * @brief Structure to hold Elgato light state information.
 */
struct ElgatoLight {
    int on = 0;           // 0 = off, 1 = on
    int brightness = 0;   // 0-100
    int temperature = 0;  // 143-344 (color temperature in mireds)
    std::string error;    // Error message if request fails
};

/**
 * @brief Structure to hold complete Elgato lights response.
 */
struct ElgatoLightsResponse {
    int numberOfLights = 0;
    ElgatoLight lights[1]; // Supporting single light for now
    std::string error;
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
DeviceInfo sendHttpGetRequest(const std::string &host, const int &port, const std::string &path);

/**
 * @brief Sends an HTTP PUT request with JSON body to a specified host, port, and path.
 *
 * @param host The hostname or IP address.
 * @param port The port number (usually 9123 for Elgato).
 * @param path The path of the resource.
 * @param json_body The JSON string to send in the request body.
 * @return A string containing the response body, or an empty string on error.
 */
std::string sendHttpPutRequest(const std::string &host, const int &port, const std::string &path, const std::string &json_body);

// --- Elgato API Functions ---

/**
 * @brief Sets the light state (on/off, brightness, temperature) for an Elgato light.
 *
 * @param ip The IP address of the Elgato light.
 * @param brightness The brightness level (0-100).
 * @param temperature Optional color temperature in mireds (143-344). If not provided, temperature won't be changed.
 * @return ElgatoLight struct with the updated state, or error message in 'error' field.
 */
ElgatoLight setLight(const std::string &ip, int brightness, std::optional<int> temperature = std::nullopt);

/**
 * @brief Gets the current light state from an Elgato light.
 *
 * @param ip The IP address of the Elgato light.
 * @return ElgatoLight struct with the current state, or error message in 'error' field.
 */
ElgatoLight getLight(const std::string &ip);

/**
 * @brief Gets the accessory info from an Elgato device.
 *
 * @param ip The IP address of the Elgato device.
 * @return DeviceInfo struct with device information, or error message in 'error' field.
 */
DeviceInfo getInfo(const std::string &ip);

/**
 * @brief Sets the display name for an Elgato device.
 *
 * @param ip The IP address of the Elgato device.
 * @param name The new display name to set.
 * @return true if successful, false otherwise.
 */
bool setDeviceName(const std::string &ip, const std::string &name);

#endif // HTTP_CLIENT_H