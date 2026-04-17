# ESP32 Elgato Light Control

ESP-IDF firmware for an ESP32 (configured for Seeed XIAO ESP32-C6) that discovers Elgato lights on your LAN and exposes a local HTTP API to control them by group.

## mDNS name

This device advertises itself as:

- `esp32-elgato-lights.local`

It also queries `_elg._tcp.local` to discover Elgato devices.

## What this project does

- Connects to Wi-Fi using credentials stored in NVS.
- Discovers Elgato lights on the local network with mDNS.
- Fetches and caches accessory information for discovered devices.
- Exposes an HTTP server on port `80` for listing devices, managing groups, and controlling lights.
- Persists light groups in NVS.

## Build target

- PlatformIO environment: `seeed_xiao_esp32c6`
- Framework: `espidf`

## Configuration

Wi-Fi credentials are read from NVS namespace `elights` with keys:

- `WIFI_SSID`
- `WIFI_PASS`

If either key is missing, the firmware logs an error and halts.

## Local HTTP API (served by ESP32)

Base URL:

- `http://<esp32-ip>/`

### 1) List discovered devices

- **Method**: `GET`
- **Path**: `/lights/all`
- **Description**: Returns cached JSON list of discovered Elgato devices.
- **Success response**: JSON array of device objects, each with fields:
  - `serialNumber`, `ip`, `productName`, `hardwareBoardType`, `hardwareRevision`, `macAddress`, `firmwareBuildNumber`, `firmwareVersion`, `displayName`
- **Error responses**:
  - `503 Service Unavailable` with `{"error":"Server cache not initialized"}`
  - `503 Service Unavailable` with `{"error":"Cache busy"}`

### 2) List light groups

- **Method**: `GET`
- **Path**: `/lights/group`
- **Description**: Returns all configured groups.
- **Success response**:

```json
{
  "groups": [
    {
      "groupName": "studio",
      "serialNumbers": ["SERIAL1", "SERIAL2"],
      "deviceCount": 2
    }
  ],
  "totalGroups": 1
}
```

### 3) Create or update a group

- **Method**: `PUT`
- **Path**: `/lights/group`
- **Description**: Creates or replaces a group mapping to Elgato serial numbers.
- **Request body**:

```json
{
  "groupName": "studio",
  "serialNumbers": ["SERIAL1", "SERIAL2"]
}
```

- **Success response**:

```json
{
  "success": true,
  "groupName": "studio",
  "deviceCount": 2
}
```

- **Error responses**:
  - `400 Bad Request` with `{"error":"Failed to read request body"}`
  - `400 Bad Request` with `{"error":"Invalid JSON"}`
  - `400 Bad Request` with `{"error":"Missing or invalid groupName or serialNumbers"}`
  - `400 Bad Request` with `{"error":"serialNumbers array is empty"}`

### 4) Control all lights in a group

- **Method**: `PUT`
- **Path**: `/lights`
- **Description**: Applies brightness and temperature to every device in the target group.
- **Request body**:

```json
{
  "group": "studio",
  "light": {
    "brightness": 50,
    "temperature": 200
  }
}
```

- **Validation constraints**:
  - `brightness`: `0..100`
  - `temperature`: required for this endpoint and must be `143..344`

- **Success response**:

```json
{
  "groupName": "studio",
  "totalDevices": 2,
  "successCount": 2,
  "failCount": 0,
  "results": [
    {
      "serial": "SERIAL1",
      "displayName": "Key Light",
      "success": true,
      "brightness": 50,
      "temperature": 200
    }
  ]
}
```

- **Error responses**:
  - `400 Bad Request` with `{"error":"Failed to read request body"}`
  - `400 Bad Request` with `{"error":"Invalid JSON"}`
  - `400 Bad Request` with `{"error":"Missing or invalid 'group' or 'light' fields"}`
  - `400 Bad Request` with `{"error":"Missing or invalid brightness or temperature in light object"}`
  - `404 Not Found` with `{"error":"Group not found or empty"}`

### 5) Turn off all discovered lights

- **Method**: `PUT`
- **Path**: `/lights/off`
- **Description**: Sends brightness `0` to all currently known devices.
- **Success response**:

```json
{
  "totalDevices": 2,
  "successCount": 2,
  "failCount": 0,
  "results": [
    {
      "serial": "SERIAL1",
      "displayName": "Key Light",
      "success": true
    }
  ]
}
```

- **Error responses**:
  - `404 Not Found` with `{"error":"No devices found"}`

## Outbound API requests (sent by ESP32 to Elgato devices)

The firmware acts as an HTTP client against each discovered Elgato light on port `9123`.

### A) Read accessory info

- **Method**: `GET`
- **URL**: `http://<light-ip>:9123/elgato/accessory-info`
- **Used for**: Device discovery enrichment and metadata cache.
- **Parsed fields**: `productName`, `hardwareBoardType`, `hardwareRevision`, `macAddress`, `firmwareBuildNumber`, `firmwareVersion`, `serialNumber`, `displayName`.

### B) Read light state

- **Method**: `GET`
- **URL**: `http://<light-ip>:9123/elgato/lights`
- **Used for**: Reading current on/brightness/temperature state.

### C) Set light state

- **Method**: `PUT`
- **URL**: `http://<light-ip>:9123/elgato/lights`
- **Content-Type**: `application/json`
- **Request body shape**:

```json
{
  "numberOfLights": 1,
  "lights": [
    {
      "on": 1,
      "brightness": 50,
      "temperature": 200
    }
  ]
}
```

Notes:

- `on` is derived from brightness (`0` -> off, `>0` -> on).
- `temperature` is optional in requests from this firmware.

### D) Set device display name

- **Method**: `PUT`
- **URL**: `http://<light-ip>:9123/elgato/accessory-info`
- **Content-Type**: `application/json`
- **Request body**:

```json
{
  "displayName": "New Name"
}
```

## Notes

- Server listens on port `80`.
- Elgato client requests use a `2000 ms` timeout.
- Group data is serialized and stored in NVS key `light_groups` under namespace `elights`.
