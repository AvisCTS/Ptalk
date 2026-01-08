# Quick Start: WebSocket Configuration

## For Developers - Integration Checklist

### Step 1: Setup NetworkManager with Managers
```cpp
// In DeviceProfile::setup() or AppController
network_mgr->setManagers(audio_mgr.get(), display_mgr.get());

// Optional: Register callback for config updates
network_mgr->onConfigUpdate([](const std::string &key, const std::string &value) {
    ESP_LOGI("APP", "Config updated: %s = %s", key.c_str(), value.c_str());
});
```

### Step 2: Device Automatically Sends Handshake
When WS connects, device automatically sends:
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

### Step 3: Server Sends Config Commands
```python
# Send to specific device by ID
await send_config_to_device("A1B2C3D4E5F6", {
    "cmd": "set_volume",
    "volume": 75
})
```

### Step 4: Device Processes and Responds
Device automatically processes command and sends response:
```json
{
  "status": "ok",
  "volume": 75,
  "device_id": "A1B2C3D4E5F6"
}
```

## Command Reference

| Command | Parameters | Effect | Reboot |
|---------|-----------|--------|--------|
| set_volume | volume (0-100) | Real-time volume change | No |
| set_brightness | brightness (0-100) | Real-time display brightness | No |
| set_device_name | device_name | Update device name | No |
| set_wifi | ssid, password | Update WiFi credentials | Yes |
| request_status | (none) | Query device status | No |
| force_listen | (none) | Start microphone input | No |
| reboot | (none) | Restart device | Yes |

## Debugging

### Device Logs
```
I (12345) NetworkManager: Device handshake sent to server
I (12346) NetworkManager: Processing config command: set_volume
I (12347) NetworkManager: Applying volume config: 75%
```

### Server-Side Verification
```python
# Always check device_id in response matches sent command
received_msg = await websocket.receive_text()
if received_msg["device_id"] != target_device_id:
    print("Response from different device!")
```

## Troubleshooting

### Device doesn't send handshake
- ✅ Check WS connection successful (state = ONLINE)
- ✅ Check device_id in logs
- ✅ Verify server receives on WS endpoint

### Device doesn't respond to commands
- ✅ Verify command JSON is valid
- ✅ Check command name matches (set_volume, not setVolume)
- ✅ Check device is still connected (check logs)
- ✅ Verify parameters are correct type (number, string, etc.)

### Settings not persisting
- ⚠️ Volume/brightness: Not saved to NVS (TODO)
- ⚠️ Device name: Not saved to NVS (TODO)
- ✅ WiFi: Saved to NVS automatically

## Performance Notes

- Commands processed immediately (< 100ms)
- JSON parsing overhead minimal with cJSON
- No blocking operations on device
- Response sent within 500ms
- WiFi config waits 1s before reboot for response delivery

## Security Considerations

### Current (Development)
⚠️ No authentication required
⚠️ No rate limiting
⚠️ No command validation

### Recommended for Production
- [ ] Add device token/authentication
- [ ] Implement rate limiting on commands
- [ ] Validate parameter ranges
- [ ] Log all config changes to database
- [ ] Add user authorization checks
- [ ] Encrypt sensitive parameters in transit

## Examples

### Python Server - Device Control
```python
from fastapi import WebSocket
from typing import Dict
import json

class DeviceManager:
    def __init__(self):
        self.devices: Dict[str, WebSocket] = {}
    
    async def register_device(self, device_id: str, ws: WebSocket):
        self.devices[device_id] = ws
        print(f"Device {device_id} registered")
    
    async def send_command(self, device_id: str, cmd: str, **params):
        if device_id not in self.devices:
            print(f"Device {device_id} not connected")
            return False
        
        message = {"cmd": cmd, **params}
        try:
            await self.devices[device_id].send_json(message)
            return True
        except Exception as e:
            print(f"Failed to send command: {e}")
            return False
    
    async def set_volume(self, device_id: str, volume: int):
        return await self.send_command(device_id, "set_volume", volume=volume)
    
    async def set_brightness(self, device_id: str, brightness: int):
        return await self.send_command(device_id, "set_brightness", brightness=brightness)
    
    async def query_status(self, device_id: str):
        return await self.send_command(device_id, "request_status")

# Usage
manager = DeviceManager()

# Register device on handshake
@router.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    try:
        while True:
            data = await websocket.receive_text()
            msg = json.loads(data)
            
            if msg.get("cmd") == "device_handshake":
                device_id = msg["device_id"]
                await manager.register_device(device_id, websocket)
            
            # Handle responses...
    except Exception as e:
        print(f"Error: {e}")

# Control devices from anywhere
async def update_all_volumes(volume: int):
    for device_id in manager.devices.keys():
        await manager.set_volume(device_id, volume)
```

### CLI Example
```bash
# Send command to device
curl -X POST http://localhost:8000/api/devices/A1B2C3D4E5F6/config \
  -H "Content-Type: application/json" \
  -d '{"cmd": "set_volume", "volume": 75}'

# Query device status
curl http://localhost:8000/api/devices/A1B2C3D4E5F6/status

# Configure multiple devices
curl -X POST http://localhost:8000/api/devices/bulk-config \
  -d '{"device_ids": ["A1B2C3D4E5F6", "B2C3D4E5F6A1"], "config": {"cmd": "set_brightness", "brightness": 80}}'
```

## Related Documentation

- Full Protocol: [WEBSOCKET_CONFIG_PROTOCOL.md](WEBSOCKET_CONFIG_PROTOCOL.md)
- Implementation Details: [WEBSOCKET_CONFIG_IMPLEMENTATION.md](WEBSOCKET_CONFIG_IMPLEMENTATION.md)
- NetworkManager API: [src/system/NetworkManager.hpp](../src/system/NetworkManager.hpp)
- Protocol Definitions: [include/system/WSConfig.hpp](../include/system/WSConfig.hpp)
