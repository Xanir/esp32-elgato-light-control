#include "cache_lights.h"
#include "nvs_helper.h"
#include "esp_log.h"
#include <sstream>

static const char* TAG = "LIGHT_CACHE";
static const std::string NVS_LIGHT_GROUPS_KEY = "light_groups";

void LightGroupCache::init() {
    loadFromNVS();
}

void LightGroupCache::addGroup(const std::string &groupName, const std::vector<std::string> &serialNumbers, bool saveToNVS) {
    ESP_LOGI(TAG, "Adding group '%s' with %d devices", groupName.c_str(), serialNumbers.size());
    groupMap[groupName] = serialNumbers;
    ESP_LOGI(TAG, "Group map now has %d groups", groupMap.size());
    if (saveToNVS) {
        this->saveToNVS();
        ESP_LOGI(TAG, "Group '%s' saved successfully", groupName.c_str());
    } else {
        ESP_LOGI(TAG, "Group '%s' added to cache (not yet persisted)", groupName.c_str());
    }
}

void LightGroupCache::removeGroup(const std::string &groupName) {
    groupMap.erase(groupName);
    saveToNVS();
}

std::vector<std::string> LightGroupCache::getGroup(const std::string &groupName) const {
    auto it = groupMap.find(groupName);
    if (it != groupMap.end()) {
        return it->second;
    }
    return std::vector<std::string>();
}

bool LightGroupCache::hasGroup(const std::string &groupName) const {
    return groupMap.find(groupName) != groupMap.end();
}

std::map<std::string, std::vector<std::string>> LightGroupCache::getAllGroups() const {
    return groupMap;
}

void LightGroupCache::clear() {
    groupMap.clear();
    saveToNVS();
}

void LightGroupCache::saveToNVS() {
    ESP_LOGI(TAG, "Serializing %d groups for NVS storage", groupMap.size());
    std::string serialized = serializeGroups();
    ESP_LOGI(TAG, "Serialized data length: %d bytes", serialized.length());
    ESP_LOGD(TAG, "Serialized data: %s", serialized.c_str());

    bool success = set_nvs_string_value(APP_NVS_NS, NVS_LIGHT_GROUPS_KEY, serialized);
    if (success) {
        ESP_LOGI(TAG, "Successfully saved groups to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to save groups to NVS!");
    }
}

void LightGroupCache::loadFromNVS() {
    std::string data = get_nvs_string_value(NVS_LIGHT_GROUPS_KEY);
    if (!data.empty()) {
        deserializeGroups(data);
    }
}

std::string LightGroupCache::serializeGroups() const {
    std::ostringstream oss;

    for (const auto &group : groupMap) {
        // Format: groupName|serial1,serial2,serial3;nextGroup|...
        oss << group.first << "|";
        for (size_t i = 0; i < group.second.size(); ++i) {
            oss << group.second[i];
            if (i < group.second.size() - 1) {
                oss << ",";
            }
        }
        oss << ";";
    }

    return oss.str();
}

void LightGroupCache::deserializeGroups(const std::string &data) {
    groupMap.clear();

    std::istringstream iss(data);
    std::string groupEntry;

    // Split by semicolon to get each group
    while (std::getline(iss, groupEntry, ';')) {
        if (groupEntry.empty()) continue;

        // Split by pipe to separate group name from serials
        size_t pipePos = groupEntry.find('|');
        if (pipePos == std::string::npos) continue;

        std::string groupName = groupEntry.substr(0, pipePos);
        std::string serialsStr = groupEntry.substr(pipePos + 1);

        // Split serials by comma
        std::vector<std::string> serials;
        std::istringstream serialStream(serialsStr);
        std::string serial;

        while (std::getline(serialStream, serial, ',')) {
            if (!serial.empty()) {
                serials.push_back(serial);
            }
        }

        if (!groupName.empty() && !serials.empty()) {
            groupMap[groupName] = serials;
        }
    }
}
