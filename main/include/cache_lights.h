#ifndef CACHE_LIGHTS_H
#define CACHE_LIGHTS_H

#include <map>
#include <string>
#include <vector>

class LightGroupCache {
public:
    // Initialize and load groups from NVS
    void init();

    // Add a group with its associated serial numbers
    void addGroup(const std::string &groupName, const std::vector<std::string> &serialNumbers, bool saveToNVS = true);

    // Remove a group by name
    void removeGroup(const std::string &groupName);

    // Get serial numbers for a specific group
    std::vector<std::string> getGroup(const std::string &groupName) const;

    // Check if a group exists
    bool hasGroup(const std::string &groupName) const;

    // Get all groups and their serial numbers
    std::map<std::string, std::vector<std::string>> getAllGroups() const;

    // Clear all groups
    void clear();

    // Manually trigger save to NVS
    void saveToNVS();

private:
    std::map<std::string, std::vector<std::string>> groupMap;

    // Load all groups from NVS
    void loadFromNVS();

    // Serialize group data to string for NVS storage
    std::string serializeGroups() const;

    // Deserialize group data from string
    void deserializeGroups(const std::string &data);
};

#endif // CACHE_LIGHTS_H
