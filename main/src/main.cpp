#include <string>
#include <map>
#include <vector>
#include <set>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_err.h"

// --- Local file inclues ---
#include "nvs_helper.h"
#include "wifi_helper.h"
#include "mdns_socket.h"
#include "http_requester.h"
#include "http_server.h"

// Ensure TaskConfiguration is declared
// If not present in mdns_socket.h, uncomment the forward declaration below:
// struct TaskConfiguration;

// --- LED Configuration ---
#define BLINK_GPIO GPIO_NUM_15 
#define LED_ON_LEVEL 0
#define LED_OFF_LEVEL 1

static const char* TAG = "ELIGHTS";
static std::string qname_elgato = "_elg._tcp.local";

// Keep a set of discovered mDNS IPs
static std::set<std::string> s_discovered_mdns_ips;
static std::map<std::string, DeviceInfo> device_ip_to_info_map;
static std::map<std::string, DeviceInfo> device_serial_to_info_map;

template <typename KeyType, typename ValueType>
std::set<KeyType> get_map_keys(const std::map<KeyType, ValueType>& input_map) {
    std::set<KeyType> keys;
    for (const auto& pair : input_map) {
        keys.insert(pair.first);
    }
    return keys;
}

// Wrap app_main in extern "C" for C++ compilation compatibility
extern "C" {
    void app_main(void);
}

static TaskConfiguration* create_mdns_task_conf(int sock_mdns, std::string qname_elgato, std::set<std::string> _discovered_mdns_ips, std::string hostname, std::string ip) {
    TaskConfiguration* config_sock_mdns = new TaskConfiguration();
    config_sock_mdns->sock_mdns = sock_mdns;
    config_sock_mdns->set_ip = &s_discovered_mdns_ips;
    config_sock_mdns->qname = qname_elgato;
    config_sock_mdns->our_hostname = hostname;
    config_sock_mdns->our_ip = ip;

    return config_sock_mdns;
}

void mdns_socket_task_wrapper(void* pvParameters) {
    int sock_mdns = ((TaskConfiguration*)pvParameters)->sock_mdns;
    std::set<std::string> *set_ip_ptr = ((TaskConfiguration*)pvParameters)->set_ip;
    std::set<std::string> &set_ip = *set_ip_ptr;
    std::string qname = ((TaskConfiguration*)pvParameters)->qname;
    std::string our_hostname = ((TaskConfiguration*)pvParameters)->our_hostname;
    std::string our_ip = ((TaskConfiguration*)pvParameters)->our_ip;

    while (1) {
        ESP_LOGI(TAG, "Sending mDNS PTR query for %s", qname.c_str());
        ssize_t sent = send_mdns_ptr_query(sock_mdns, qname);
        if (sent > 0) {
            ESP_LOGI(TAG, "mDNS query sent successfully (%d bytes)", (int)sent);
        } else {
            ESP_LOGW(TAG, "Failed to send mDNS query");
        }

        // Unified task handles both service discovery responses AND query responses
        mdns_socket_task(sock_mdns, qname, set_ip, our_hostname, our_ip);

        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}

void app_main(void) {
    // 0. Configure the onboard LED
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL<<BLINK_GPIO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE; 
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE; 
    gpio_config(&io_conf);

    gpio_set_level(BLINK_GPIO, LED_OFF_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BLINK_GPIO, LED_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BLINK_GPIO, LED_OFF_LEVEL);

    // Wait a seconds to allow to dewbugging tools to connect
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 1. Initialize Non-Volatile Storage (NVS)
    initialize_nvs();

    // Reduce WiFi logging verbosity
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);

    // 2. Read Wi-Fi credentials from NVS
    std::string wifi_ssid = get_nvs_string_value("WIFI_SSID");
    std::string wifi_password = get_nvs_string_value("WIFI_PASS");

    // 3. Validate credentials before attempting connection
    if (wifi_ssid.empty() || wifi_password.empty()) {
        ESP_LOGE(TAG, "WiFi credentials not found in NVS. Please set WIFI_SSID and WIFI_PASS.");
        vTaskDelete(NULL);
        return;
    }

    // 4. Initialize Wi-Fi Connection
    esp_netif_t* netif = wifi_init_station(wifi_ssid, wifi_password);
    if (netif == NULL) {
        ESP_LOGE(TAG, "Failed to initialize WiFi");
        vTaskDelete(NULL);
        return;
    }
    const char* system_ip = get_wifi_ip();
    ESP_LOGI(TAG, "Device IP: %s", system_ip);

    gpio_set_level(BLINK_GPIO, LED_ON_LEVEL);
    ESP_LOGI(TAG, "WiFi initialization complete");

    // 6. Create the mDNS socket
    ESP_LOGI(TAG, "Setting up mDNS socket...");
    static int sock_mdns = mdns_setup_socket();
    if (sock_mdns < 0) {
        ESP_LOGE(TAG, "mDNS socket setup failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "mDNS socket created successfully");

    // Prepare task configuration and provide a pointer to the shared set so
    // the mdns task updates `s_discovered_mdns_ips` directly.
    ESP_LOGI(TAG, "Creating mDNS task...");
    static TaskConfiguration* config_sock_mdns = create_mdns_task_conf(sock_mdns, qname_elgato, s_discovered_mdns_ips, "esp32-elights.local", system_ip);
    BaseType_t mdns_task_result = xTaskCreatePinnedToCore(mdns_socket_task_wrapper, "mdns_task", 3600, config_sock_mdns, 1, NULL, 0);
    if (mdns_task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mDNS task");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "mDNS task created successfully");

    // 5. Start HTTP server
    static int http_sock = http_server_start(&device_serial_to_info_map);
    if (http_sock < 0) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "HTTP server started successfully");

    int announcement_counter = 0;
    while (1) {
        std::set<std::string> known_devices = get_map_keys(device_ip_to_info_map);
        std::vector<std::string> needed_ids;
        std::set_difference(
            s_discovered_mdns_ips.begin(), s_discovered_mdns_ips.end(),
            known_devices.begin(), known_devices.end(),
            std::back_inserter(needed_ids)
        );

        if (!needed_ids.empty()) {
            ESP_LOGI(TAG, "Found %d new devices to query", needed_ids.size());
        }

        for (const std::string& item : needed_ids) {
            ESP_LOGI(TAG, "Getting light data for %s", item.c_str());
            DeviceInfo info = sendHttpGetRequest(item, 9123, "/elgato/accessory-info");

            if (info.error.empty()) {
                device_ip_to_info_map[item] = info;
                device_serial_to_info_map[info.serialNumber] = info;
                ESP_LOGI(TAG, "Successfully added device: %s", info.serialNumber.c_str());
            } else {
                ESP_LOGW(TAG, "Failed to get info for %s: %s", item.c_str(), info.error.c_str());
            }
        }

        vTaskDelay(pdMS_TO_TICKS(9000));

        // Send periodic mDNS announcement (every 5th iteration = every 45 seconds)
        announcement_counter++;
        if (announcement_counter >= 5) {
            ESP_LOGI(TAG, "Sending periodic mDNS announcement");
            send_mdns_announcement(sock_mdns, "_http._tcp.local", "ESP32 Elgato Light Control", "esp32-elights.local", system_ip, 80, {});
            send_mdns_a_record(sock_mdns, "esp32-elights.local", system_ip);
            announcement_counter = 0;
        }
    }
}