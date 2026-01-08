# PTalk WebSocket Configuration Guide for Server

## Overview

PTalk devices support real-time configuration via WebSocket. After a device connects and sends a `device_handshake`, the server can send JSON commands to configure settings (volume, brightness, device name), query status, trigger reboot, or request OTA updates.

**Key Points:**
- Device identifies itself with `device_id` (MAC address) on handshake
- All config commands return JSON responses with `status` and `device_id`
- WiFi credentials are **NOT** accepted over WS (use BLE/portal instead)
- Volume, brightness, device name persist across reboot (stored in NVS)
- Each command is optional; send only what you need

---

## Commands Quick Reference

| Command | Purpose | Params | Notes |
|---------|---------|--------|-------|
| `device_handshake` | Device → Server handshake | - | Sent by device on WS connect; not a server command |
| `request_status` | Query device status | - | Returns all current settings + battery, connectivity, uptime |
| `set_volume` | Set speaker volume | `volume` (0-100) | Applied immediately, persisted |
| `set_brightness` | Set display brightness | `brightness` (0-100) | Applied immediately, persisted |
| `set_device_name` | Set device name | `device_name` (string) | Persisted |
| `reboot` | Reboot device | - | Device reboots after 500ms ack |
| `request_ota` | Trigger OTA update | `version` (optional) | Device will request firmware binary over WS |
| `set_wifi` | Set WiFi ❌ NOT SUPPORTED | - | Device responds `not_supported`; use BLE instead |

---

## Device Handshake (Receive from Device)

When device connects to WS, it immediately sends:

```json
{
  "cmd": "device_handshake",
  "device_id": "A1B2C3D4E5F6",
  "firmware_version": "1.0.1",
  "device_name": "PTalk",
  "battery_percent": 85,
  "connectivity_state": "ONLINE"
}
```

**Use `device_id` to map this device in your server database for future commands.**

---

## Command Examples & Responses

### 1. Request Status

**Send:**
```json
{
  "cmd": "request_status"
}
```

**Receive:**
```json
{
  "status": "ok",
  "device_id": "A1B2C3D4E5F6",
  "device_name": "PTalk",
  "battery_percent": 85,
  "connectivity_state": "ONLINE",
  "firmware_version": "1.0.1",
  "volume": 60,
  "brightness": 100,
  "uptime_sec": 3600
}
```

### 2. Set Volume

**Send:**
```json
{
  "cmd": "set_volume",
  "volume": 75
}
```

**Receive:**
```json
{
  "status": "ok",
  "volume": 75,
  "device_id": "A1B2C3D4E5F6"
}
```

### 3. Set Brightness

**Send:**
```json
{
  "cmd": "set_brightness",
  "brightness": 50
}
```

**Receive:**
```json
{
  "status": "ok",
  "brightness": 50,
  "device_id": "A1B2C3D4E5F6"
}
```

### 4. Set Device Name

**Send:**
```json
{
  "cmd": "set_device_name",
  "device_name": "Living Room Speaker"
}
```

**Receive:**
```json
{
  "status": "ok",
  "device_name": "Living Room Speaker",
  "device_id": "A1B2C3D4E5F6"
}
```

### 5. Reboot Device

**Send:**
```json
{
  "cmd": "reboot"
}
```

**Receive:**
```json
{
  "status": "ok",
  "message": "Rebooting...",
  "device_id": "A1B2C3D4E5F6"
}
```

Device reboots ~500ms later and reconnects with a fresh handshake.

### 6. Request OTA Update

**Send:**
```json
{
  "cmd": "request_ota",
  "version": "1.0.2"
}
```

or (request latest):
```json
{
  "cmd": "request_ota"
}
```

**Receive:**
```json
{
  "status": "ok",
  "device_id": "A1B2C3D4E5F6",
  "version": "1.0.2"
}
```

After this acknowledgment, the device is ready to receive firmware binary over the WS connection. Send firmware data as binary frames; the device will write to flash and reboot on completion.

### 7. Set WiFi (NOT Supported)

**Send:**
```json
{
  "cmd": "set_wifi",
  "ssid": "MyNetwork",
  "password": "mypassword"
}
```

**Receive:**
```json
{
  "status": "not_supported",
  "message": "WiFi config not supported over WebSocket. Use BLE.",
  "device_id": "A1B2C3D4E5F6"
}
```

WiFi provisioning must use **BLE (Bluetooth LE)** or the **captive portal**. No change is applied.

---

## Server Implementation (Python/FastAPI Example)

```python
import json
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
import uvicorn

app = FastAPI()

# Map device_id → WebSocket connection
connected_devices = {}

@app.websocket("/ws")
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
                
                print(f"✓ Device {device_id} connected")
                print(f"  Firmware: {msg.get('firmware_version')}")
                print(f"  Battery: {msg.get('battery_percent')}%")
                print(f"  Name: {msg.get('device_name')}")
                
                # Now you can send config to this device!
                
            # Device response (config ack or status)
            elif msg.get("status"):
                print(f"← Device {device_id} response: {msg}")
                # Handle response, update DB, etc.
                
    except WebSocketDisconnect:
        if device_id:
            del connected_devices[device_id]
            print(f"✗ Device {device_id} disconnected")

# Send config command to specific device
async def send_config_to_device(device_id: str, cmd: dict):
    """Send config command to connected device"""
    if device_id not in connected_devices:
        print(f"Device {device_id} not connected")
        return False
    
    try:
        ws = connected_devices[device_id]
        await ws.send_text(json.dumps(cmd))
        print(f"→ Sent to {device_id}: {cmd}")
        return True
    except Exception as e:
        print(f"Error sending to {device_id}: {e}")
        return False

# Example: HTTP endpoint to configure device
@app.post("/api/device/{device_id}/config")
async def configure_device(device_id: str, volume: int = None, brightness: int = None):
    """HTTP endpoint to send config to a device"""
    
    if volume is not None:
        cmd = {"cmd": "set_volume", "volume": min(100, max(0, volume))}
        await send_config_to_device(device_id, cmd)
    
    if brightness is not None:
        cmd = {"cmd": "set_brightness", "brightness": min(100, max(0, brightness))}
        await send_config_to_device(device_id, cmd)
    
    return {"status": "ok", "device_id": device_id}

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)
```

---

## Testing with Dummy Server CLI

A test CLI server is available in `server_test/dummy_server_cmd.py`. Commands:

```
vol 75              → set_volume
bright 80           → set_brightness
name MyDevice       → set_device_name
status              → request_status
reboot              → reboot
wifi ssid pass      → set_wifi (device will reject)
wsurl ws://...      → set_ws_url (reserved)
help                → show commands
quit                → stop server
```

Usage:
```bash
python server_test/dummy_server_cmd.py
```

Connect ESP32, then type commands to configure the device.

---

## Important Notes

1. **Device ID Linking**: Store `device_id` (MAC address) to link WS session with your device database.
2. **Persistence**: Volume, brightness, and device name are saved to NVS and survive reboots.
3. **No WiFi over WS**: WiFi provisioning is restricted to BLE or captive portal for stability.
4. **Handshake Required**: Device always sends handshake first; wait for it before sending commands.
5. **Status Query**: Use `request_status` to get current device state (battery, connectivity, settings).
6. **OTA via WS**: After `request_ota`, server sends firmware binary as WS binary frames.
7. **Reboot Recovery**: Device reconnects with a new handshake after reboot.

---

## Response Status Codes

- `"status": "ok"` → Command executed successfully
- `"status": "error"` → Command failed (e.g., OTA when WS disconnected)
- `"status": "not_supported"` → Command not allowed (e.g., set_wifi over WS)
- `"status": "invalid_command"` → Unknown command
- `"status": "invalid_param"` → Parameter out of range or invalid

---

## Persistence Behavior

| Setting | WS Config | BLE Config | Persists After Reboot |
|---------|-----------|-----------|----------------------|
| Volume | ✓ | ✓ | ✓ (NVS) |
| Brightness | ✓ | ✓ | ✓ (NVS) |
| Device Name | ✓ | ✓ | ✓ (NVS) |
| WiFi | ✗ | ✓ | ✓ (NVS) |

---

## Troubleshooting

- **Device not responding**: Verify WS connection is active (check `connectivity_state` in handshake).
- **Command returns error**: Check parameter ranges (volume/brightness 0-100, device_name non-empty).
- **Settings revert after reboot**: Ensure `nvs_flash_init()` is called on ESP32 boot (handled automatically in firmware).
- **WiFi config rejected**: Use BLE/captive portal instead.

---

## Questions?

Refer to [WEBSOCKET_CONFIG_PROTOCOL.md](WEBSOCKET_CONFIG_PROTOCOL.md) for detailed protocol specification or contact the firmware team.
