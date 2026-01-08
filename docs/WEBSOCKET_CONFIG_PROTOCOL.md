# WebSocket Real-Time Configuration Support

## Overview

The PTalk device now supports real-time configuration through WebSocket (WS) commands sent from the server. The server can link devices using their `device_id` (MAC address) and send configuration commands to control device settings such as volume, brightness, device name, status query, reboot, and OTA trigger. **WiFi credentials are NOT accepted over WS** (use BLE/captive portal instead).

## Protocol Architecture

### Device → Server: Handshake (on WS connection)

When the device connects to the WebSocket server, it immediately sends a handshake message with device identification and current status:

```json
{
  "cmd": "device_handshake",
  "device_id": "A1B2C3D4E5F6",
  "firmware_version": "1.0.0",
  "device_name": "PTalk-Device",
  "battery_percent": 85,
  "connectivity_state": "ONLINE"
}
```

**Fields:**
- `device_id`: MAC address of the device (used for server-side linking/identification)
- `firmware_version`: Current firmware version
- `device_name`: User-friendly device name
- `battery_percent`: Current battery level
- `connectivity_state`: Current connectivity state (ONLINE, OFFLINE, etc.)

### Server → Device: Configuration Commands

The server can send JSON commands to configure the device. All config commands follow the same response pattern.

#### 1. Set WiFi Configuration (NOT supported over WS)

Use BLE/captive portal for WiFi provisioning. The device will respond `not_supported` if this command is sent.

**Request:**
```json
{
  "cmd": "set_wifi",
  "ssid": "MyNetwork",
  "password": "MyPassword"
}
```

**Response:**
```json
{
  "status": "not_supported",
  "message": "WiFi config not supported over WebSocket. Use BLE.",
  "device_id": "A1B2C3D4E5F6"
}
```

**Behavior:**
- No change is applied; WiFi stays unchanged.

#### 2. Set Volume

**Request:**
```json
{
  "cmd": "set_volume",
  "volume": 75
}
```

**Response:**
```json
{
  "status": "ok",
  "volume": 75,
  "device_id": "A1B2C3D4E5F6"
}
```

**Constraints:**
- Volume range: 0-100
- Applied immediately in real-time
- No reboot required

#### 3. Set Brightness

**Request:**
```json
{
  "cmd": "set_brightness",
  "brightness": 80
}
```

**Response:**
```json
{
  "status": "ok",
  "brightness": 80,
  "device_id": "A1B2C3D4E5F6"
}
```

**Constraints:**
- Brightness range: 0-100
- Applied immediately in real-time
- No reboot required

#### 4. Set Device Name

**Request:**
```json
{
  "cmd": "set_device_name",
  "device_name": "Living Room Speaker"
}
```

**Response:**
```json
{
  "status": "ok",
  "device_name": "Living Room Speaker",
  "device_id": "A1B2C3D4E5F6"
}
```

**Behavior:**
- Updates device display name
- Stored in NVS for persistence

#### 5. Request Device Status

**Request:**
```json
{
  "cmd": "request_status"
}
```

**Response:**
```json
{
  "status": "ok",
  "device_id": "A1B2C3D4E5F6",
  "device_name": "PTalk-Device",
  "battery_percent": 85,
  "connectivity_state": "ONLINE",
  "volume": 75,
  "brightness": 80,
  "firmware_version": "1.0.0",
  "uptime_sec": 3600
}
```

#### 6. Reboot Device

**Request:**
```json
{
  "cmd": "reboot"
}
```

**Response:**
```json
{
  "status": "ok",
  "message": "Rebooting..."
}
```

**Behavior:**
- Sends acknowledgment
- Waits 500ms
- Performs ESP restart

#### 7. Request OTA Update

**Request:**
```json
{
  "cmd": "request_ota",
  "version": "1.0.2"   // optional; empty → latest
}
```

**Response:**
```json
{
  "status": "ok",
  "device_id": "A1B2C3D4E5F6",
  "version": "1.0.2"
}
```

**Behavior:**
- Device triggers `requestFirmwareUpdate(version)` over the active WS connection.
- Server should begin streaming firmware binary over WS (existing OTA chunk handling is reused).
- If WS is not connected or request fails, device responds `status: "error"` with a message.


## Integration with Application

### Initializing Configuration Support

In `DeviceProfile.cpp`:

```cpp
// After creating NetworkManager and other managers
network_mgr->setManagers(audio_mgr.get(), display_mgr.get());

// Optional: Register callback for config updates
network_mgr->onConfigUpdate([](const std::string &key, const std::string &value) {
    ESP_LOGI("APP", "Config updated: %s = %s", key.c_str(), value.c_str());
    // Handle config update notification
});
```

### Server Implementation Example (Python/FastAPI)

```python
import json
from fastapi import WebSocket, APIRouter

router = APIRouter()

# Map of device_id -> WebSocket connection
connected_devices = {}

@router.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    device_id = None
    
    try:
        while True:
            data = await websocket.receive_text()
            msg = json.loads(data)
            
            # Device handshake
            if msg.get("cmd") == "device_handshake":
                device_id = msg.get("device_id")
                connected_devices[device_id] = websocket
                print(f"Device {device_id} connected: {msg}")
                
                # Now you can send config to this device
                # Example: set volume
                config_cmd = {
                    "cmd": "set_volume",
                    "volume": 75
                }
                await websocket.send_json(config_cmd)
                
            # Device responses
            elif msg.get("status"):
                print(f"Device {device_id} response: {msg}")
                
    except Exception as e:
        print(f"Connection error: {e}")
    finally:
        if device_id:
            del connected_devices[device_id]

# Send config to specific device from anywhere in your app
async def send_config_to_device(device_id: str, config_cmd: dict):
    if device_id in connected_devices:
        ws = connected_devices[device_id]
        await ws.send_json(config_cmd)
```

### Server Configuration Workflow

1. **Device Connects:**
   - Device sends `device_handshake` with device_id
   - Server stores connection reference linked to device_id

2. **Server Sends Commands:**
   - Server can send config commands targeted at specific device_id
   - Each command includes device_id in the response for verification

3. **Device Processes:**
   - Device parses command
   - Applies configuration
   - Sends status response with device_id

4. **Server Acknowledges:**
   - Server verifies device_id matches
   - Updates internal device record
   - (Optional) Syncs to database/UI

## Error Handling

### Response Status Codes

- `"ok"`: Command executed successfully
- `"error"`: General error occurred
- `"invalid_command"`: Unknown command type
- `"invalid_param"`: Parameter validation failed
- `"not_supported"`: Feature not supported on this device
- `"device_busy"`: Device is busy (e.g., during audio streaming)

### Example Error Response

```json
{
  "status": "error",
  "message": "Invalid volume value (must be 0-100)",
  "device_id": "A1B2C3D4E5F6"
}
```

## State Machine Integration

Configuration commands that affect interaction state:

- `force_listen` → Sets state to `LISTENING` with `INPUT_SOURCE::SERVER_COMMAND`
- `reboot` → Triggers system restart

## Future Extensions

The protocol is designed for easy extension:

```cpp
// In WSConfig.hpp, add new commands:
enum class ConfigCommand : uint8_t {
    // ... existing commands ...
    SET_WS_URL = 6,        // Update WebSocket server URL (TODO)
    FORCE_SPEAK = 8,       // Force output audio (TODO)
    SET_WAKEWORD = 11,     // Configure wakeword (TODO)
    SET_MODE = 12,         // Set device mode (TODO)
};
```

## Implementation Status

### ✅ Completed
- Protocol definition (WSConfig.hpp)
- Device handshake on WS connection
- Config command parsing and routing
- Volume and brightness real-time control
- WiFi configuration with reboot
- Device name configuration
- Status query support
- Force listen command
- Reboot command

### 🔄 In Progress / TODO
- Integration with PowerManager to send actual battery percent
- Integration with AudioManager to get/set actual volume
- Integration with DisplayManager to get/set actual brightness
- NVS storage for device name persistence
- More command types (wakeword, mode, etc.)
- Database sync of device configurations
- Web UI for device management

## Files Modified/Created

- `include/system/WSConfig.hpp` - Protocol definitions
- `src/system/NetworkManager.hpp` - New public methods and callbacks
- `src/system/NetworkManager.cpp` - Implementation of config handlers
- Integration points in `src/config/DeviceProfile.cpp`
- (Future) Integration in `src/system/AudioManager.hpp/cpp`
- (Future) Integration in `src/system/DisplayManager.hpp/cpp`
