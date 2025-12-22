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

struct NetworkConfig {
    int mdns_sock;
    std::string qname_elgato = "_elg._tcp.local";
    std::string mdns_hostname = "esp32-elgato-lights.local";
    std::string wifi_ip;

    std::set<std::string> discovered_elgato_device_ips;
    std::map<std::string, DeviceInfo> device_ip_to_info_map;
    std::map<std::string, DeviceInfo> device_serial_to_info_map;
};
static NetworkConfig* net_config = new NetworkConfig();

template <typename KeyType, typename ValueType>
std::set<KeyType> get_map_keys(const std::map<KeyType, ValueType>& input_map) {
    std::set<KeyType> keys;
    for (const auto& pair : input_map) {
        keys.insert(pair.first);
    }
    return keys;
}

void mdns_socket_task_wrapper(void* pvParameters) {
    NetworkConfig* net_config = static_cast<NetworkConfig*>(pvParameters);
    ESP_LOGI(TAG, "mDNS watcher task started");
    while (1) {
        // Unified task handles both service discovery responses AND query responses
        mdns_socket_task(net_config->mdns_sock, net_config->qname_elgato, net_config->discovered_elgato_device_ips, net_config->mdns_hostname, net_config->wifi_ip);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void spam_mdns_announcements(void* pvParameters) {
    NetworkConfig* net_config = static_cast<NetworkConfig*>(pvParameters);
    ESP_LOGI(TAG, "mDNS announcement task started");
    while (1) {
        ESP_LOGI(TAG, "Sending mDNS announcement for %s", net_config->mdns_hostname.c_str());
        send_mdns_announcement(net_config->mdns_sock, "_http._tcp.local", "ESP32 Elgato Light Control", net_config->mdns_hostname, net_config->wifi_ip, 80, {});
        send_mdns_a_record(net_config->mdns_sock, net_config->mdns_hostname, net_config->wifi_ip);
        send_mdns_ptr_query(net_config->mdns_sock, net_config->qname_elgato);

        vTaskDelay(pdMS_TO_TICKS(30000)); // Announce every 10 seconds
    }
}

void process_ips(void* pvParameters) {
    while (1) {
        std::set<std::string> known_devices = get_map_keys(net_config->device_ip_to_info_map);
        std::vector<std::string> needed_ids;
        std::set_difference(
            net_config->discovered_elgato_device_ips.begin(), net_config->discovered_elgato_device_ips.end(),
            known_devices.begin(), known_devices.end(),
            std::back_inserter(needed_ids)
        );

        if (!needed_ids.empty()) {
            ESP_LOGI(TAG, "Found %d new devices to query", needed_ids.size());
        }

        for (const std::string& item : needed_ids) {
            ESP_LOGI(TAG, "Getting light data for %s", item.c_str());
            vTaskDelay(pdMS_TO_TICKS(100));
            DeviceInfo info = sendHttpGetRequest(item, 9123, "/elgato/accessory-info");

            if (info.error.empty()) {
                net_config->device_ip_to_info_map[item] = info;
                net_config->device_serial_to_info_map[info.serialNumber] = info;
                ESP_LOGI(TAG, "Successfully added device: %s", info.serialNumber.c_str());
            } else {
                ESP_LOGW(TAG, "Failed to get info for %s: %s", item.c_str(), info.error.c_str());
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void stall_app() {
    while(1) {vTaskDelay(pdMS_TO_TICKS(5000));}
}

void init_led() {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL<<BLINK_GPIO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE; 
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE; 
    gpio_config(&io_conf);
}

// Wrap app_main in extern "C" for C++ compilation compatibility
extern "C" {
    void app_main(void);
}

void app_main(void) {
    // Print startup banner immediately
    printf("\n\n========================================\n");
    printf("ESP32 Elgato Light Control Starting...\n");
    printf("========================================\n\n");

    ESP_LOGI(TAG, "System starting up...");

    // 0. Configure the onboard LED
    init_led();
    gpio_set_level(BLINK_GPIO, LED_OFF_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BLINK_GPIO, LED_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BLINK_GPIO, LED_OFF_LEVEL);
    ESP_LOGI(TAG, "LED initialized");

    // Wait a seconds to allow to dewbugging tools to connect
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 1. Initialize Non-Volatile Storage (NVS)
    ESP_LOGI(TAG, "Initializing NVS...");
    initialize_nvs();
    ESP_LOGI(TAG, "NVS initialized");

    // Reduce WiFi logging verbosity
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);

    // 2. Read Wi-Fi credentials from NVS
    ESP_LOGI(TAG, "Reading WiFi credentials from NVS...");
    std::string wifi_ssid = get_nvs_string_value("WIFI_SSID");
    std::string wifi_password = get_nvs_string_value("WIFI_PASS");

    // 3. Validate credentials before attempting connection
    if (wifi_ssid.empty() || wifi_password.empty()) {
        ESP_LOGE(TAG, "WiFi credentials not found in NVS. Please set WIFI_SSID and WIFI_PASS.");
        ESP_LOGE(TAG, "System halted. Please flash credentials and restart.");
        // Terminate further execution
        stall_app();
    }
    ESP_LOGI(TAG, "Credentials found. SSID: %s", wifi_ssid.c_str());

    // 4. Initialize Wi-Fi Connection
    ESP_LOGI(TAG, "Starting WiFi connection...");
    esp_netif_t* netif = wifi_init_station(wifi_ssid, wifi_password);
    if (netif == NULL) {
        ESP_LOGE(TAG, "Failed to initialize WiFi - halting system");
        // Restart application so it can attempt to
        stall_app();
    }
    net_config->wifi_ip = get_wifi_ip();
    ESP_LOGI(TAG, "WiFi connected! Device IP: %s", net_config->wifi_ip.c_str());

    gpio_set_level(BLINK_GPIO, LED_ON_LEVEL);
    ESP_LOGI(TAG, "WiFi initialization complete");

    // 5. Create the mDNS socket
    ESP_LOGI(TAG, "Setting up mDNS socket...");
    net_config->mdns_sock = mdns_setup_socket();
    ESP_LOGI(TAG, "mDNS socket created successfully");

    // Prepare task configuration and provide a pointer to the shared set so
    // the mdns task updates `s_discovered_mdns_ips` directly.
    ESP_LOGI(TAG, "Creating mDNS tasks...");
    if (xTaskCreatePinnedToCore(mdns_socket_task_wrapper, "mdns_watcher_task", 4096, net_config, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mDNS watcher task");
        stall_app();
    }
    if (xTaskCreatePinnedToCore(spam_mdns_announcements, "mdns_announcements", 4096, net_config, 9, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mDNS announcement task");
        stall_app();
    }
    ESP_LOGI(TAG, "mDNS tasks created successfully");

    if (xTaskCreatePinnedToCore(process_ips, "process_ips", 4096, NULL, 7, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create IP resolution task");
        stall_app();
    }
    ESP_LOGI(TAG, "IP resolution task created successfully");

    // 6. Start HTTP server
    ESP_LOGI(TAG, "Starting HTTP server...");
    static httpd_handle_t http_server = http_server_start(&net_config->device_ip_to_info_map);
    if (http_server == NULL) {
        ESP_LOGE(TAG, "HTTP server failed to start - halting");
        stall_app();
    }
    ESP_LOGI(TAG, "HTTP server started successfully on port 80");

    ESP_LOGI(TAG, "Entering main loop - monitoring for Elgato devices");
    while (1) {

        ESP_LOGI(TAG, "Devices: %d, Free heap: %lu bytes", 
                net_config->device_ip_to_info_map.size(),
                esp_get_free_heap_size());

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}