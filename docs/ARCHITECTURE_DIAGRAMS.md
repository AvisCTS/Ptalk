# WebSocket Configuration System - Architecture Diagrams

## 1. Overall System Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                      Server / Backend                           │
│              (App, Dashboard, or API Gateway)                   │
│                                                                  │
│  Maintains device_id → WebSocket connection mapping             │
│  Sends configuration commands to specific devices               │
└────────────────────────────┬─────────────────────────────────┘
                             │
                   WebSocket Connection
                    (JSON over TCP)
                             │
         ┌──────────────────┴──────────────────┐
         │   Device Handshake on Connect       │
         │   {"cmd": "device_handshake", ...}  │
         │                                      │
         │   Config Commands                   │
         │   {"cmd": "set_volume", ...}        │
         │                                      │
         │   Device Responses                  │
         │   {"status": "ok", ...}             │
         │                                      │
         └──────────────────┬──────────────────┘
                             │
                             ▼
        ┌────────────────────────────────────────┐
        │       ESP32 Device (PTalk)              │
        │                                         │
        │  ┌──────────────────────────────────┐  │
        │  │  Network Manager                  │  │
        │  │  ├─ sendDeviceHandshake()        │  │
        │  │  ├─ handleConfigCommand()        │  │
        │  │  ├─ applyVolumeConfig()          │  │
        │  │  ├─ applyBrightnessConfig()      │  │
        │  │  ├─ applyWiFiConfig()            │  │
        │  │  └─ getCurrentStatusJson()       │  │
        │  └──────┬──────────────────────────┘  │
        │         │                              │
        │    ┌────┴─────┬──────────┬──────┐     │
        │    │          │          │      │     │
        │    ▼          ▼          ▼      ▼     │
        │  Audio      Display    WiFi  State  │
        │  Manager    Manager   Service Manager│
        │  (Volume)  (Bright)   (Config) (UI) │
        │                                      │
        └──────────────────────────────────────┘
```

## 2. Message Flow Diagram

```
┌─────────────────┐                          ┌──────────────┐
│  WebSocket      │                          │   Device     │
│  Server         │                          │   (PTalk)    │
└─────────────────┘                          └──────────────┘
        │                                           │
        │◄──────── WS Connected ─────────────────────┤
        │                                            │
        │◄──── device_handshake (+ device_id) ───────┤
        │                                            │
        │ {"cmd": "device_handshake",               │
        │  "device_id": "A1B2C3D4E5F6",             │
        │  "firmware_version": "1.0.0", ...}        │
        │                                            │
        ├─────── set_volume (cmd) ───────────────────→
        │ {"cmd": "set_volume", "volume": 75}       │
        │                                       ┌────┤
        │                                       │    │ Parse JSON
        │                                       │    │ Validate
        │                                       │    │ Apply config
        │                                       │    │ Build response
        │                                       └────┤
        │◄─── response (with device_id) ─────────────┤
        │ {"status": "ok",                           │
        │  "volume": 75,                             │
        │  "device_id": "A1B2C3D4E5F6"}              │
        │                                            │
        │ Store in device record:                    │
        │ device_id → volume=75 ✓                    │
        │                                            │
        ├─────── set_brightness (cmd) ──────────────→
        │ {"cmd": "set_brightness", ...}            │
        │                                       ┌────┤
        │                                       │    │ Process
        │                                       └────┤
        │◄─── response ──────────────────────────────┤
        │                                            │
```

## 3. Configuration Command Processing

```
Incoming WS Message
        │
        ▼
Parse JSON
        │
        ├─ Invalid JSON? ──→ Log error, discard
        │
        ▼
Has "cmd" field?
        │
        ├─ No ──→ Try emotion code handling (backward compat)
        │
        ├─ Yes ──→ Extract command string
        │          │
        │          ▼
        │         Map to ConfigCommand enum
        │          │
        │          ├─ SET_AUDIO_VOLUME ──→ applyVolumeConfig()
        │          ├─ SET_BRIGHTNESS ──→ applyBrightnessConfig()
        │          ├─ SET_DEVICE_NAME ──→ applyDeviceNameConfig()
        │          ├─ SET_WIFI ──→ applyWiFiConfig() + reboot
        │          ├─ REQUEST_STATUS ──→ getCurrentStatusJson()
        │          ├─ FORCE_LISTEN ──→ Set LISTENING state
        │          ├─ REBOOT ──→ Restart device
        │          └─ ... more commands
        │
        │          ▼
        │         Build JSON response
        │         {"status": "ok/error", ...}
        │          │
        │          ▼
        │         Include device_id
        │         {"status": "ok", "device_id": "..."}
        │          │
        │          ▼
        │         Send response
        │         ws->sendText(response)
        │
        ▼
Done
```

## 4. Device State Transitions

```
BOOTING
  │
  ├─ WiFi connects → CONNECTING_WIFI
  │  │
  │  └─ WiFi connected → CONNECTING_WS
  │     │
  │     └─ WS connects → [HANDSHAKE SENT] → ONLINE
  │        │
  │        ├─ Can receive config commands
  │        ├─ Can receive emotion codes
  │        └─ Can receive force_listen
  │
  └─ Config received (set_wifi)
     │
     ├─ Save to NVS
     ├─ Send response
     ├─ Wait 1s
     └─ esp_restart() → BOOTING (cycle)
```

## 5. Response Status Mapping

```
Command Result → Response Status

OK ─────────────────────────→ {"status": "ok", ...}
Invalid JSON ────────────────→ Log error (no response)
Missing command ─────────────→ Log error (no response)
Parameter out of range ──────→ {"status": "invalid_param", ...}
Unknown command ─────────────→ {"status": "invalid_command", ...}
Operation failed ────────────→ {"status": "error", ...}
Device busy ──────────────────→ {"status": "device_busy", ...}
Not supported ────────────────→ {"status": "not_supported", ...}

All successful responses include:
  - device_id (for server verification)
  - status (ok/error)
  - Relevant data (volume, brightness, etc.)
```

## 6. Device ID Lifecycle

```
Device Boot
  │
  └─ Get MAC address
     └─ Convert to hex (A1B2C3D4E5F6)
        └─ Store as device_id
           │
           └─ WiFi connects
              │
              └─ WS connects
                 │
                 ├─ Send in handshake ✓
                 │  "device_id": "A1B2C3D4E5F6"
                 │
                 └─ Include in every response ✓
                    "device_id": "A1B2C3D4E5F6"

Server:
  Device connects
  │
  ├─ Receive: {"cmd": "device_handshake", "device_id": "A1B2C3D4E5F6"}
  │
  ├─ Extract device_id
  │
  ├─ Store: connected_devices["A1B2C3D4E5F6"] = ws_connection
  │
  └─ Now can send commands to: connected_devices["A1B2C3D4E5F6"]

  When device responds:
  ├─ Parse: {"status": "ok", "device_id": "A1B2C3D4E5F6"}
  │
  ├─ Verify device_id matches sent command
  │
  └─ Update device record
```

## 7. Configuration Change Flow

```
Server                          Device                    Components

send_command("set_volume", 75)
                      ──────────→ receive JSON
                                 │
                                 ├─ Parse "cmd": "set_volume"
                                 ├─ Extract "volume": 75
                                 │
                                 ├─ Validate (0-100) ✓
                                 │
                                 ├─ applyVolumeConfig(75)
                                 │  │
                                 │  ├─ Send response
                                 │  │
                                 │  └─ Call callback
                                 │     │
                                 │     └─ on_config_update_cb("volume", "75")
                                 │        │
                                 │        └─ App can handle update
                                 │
                                 ├─ If manager set:
                                 │  └─ audio_manager->setVolume(75)
                                 │     │
                                 │     └─ Update speaker hardware
                                 │
                      ←──────────┤ Send: {"status": "ok", "volume": 75}
                                 │
receive_response()
│
└─ Parse device_id
   └─ Update device record
      └─ UI reflects new state
```

## 8. Error Scenarios

```
Scenario 1: Malformed JSON
  Input: "{ invalid json"
  ├─ cJSON_Parse() fails
  ├─ Log: "Invalid JSON config command: { invalid json"
  └─ Discard (no response sent)

Scenario 2: Missing Command
  Input: {"device_id": "123"}
  ├─ cJSON_GetObjectItem("cmd") returns NULL
  ├─ Check fails
  ├─ Log: "Config command missing..."
  └─ Discard (no response sent)

Scenario 3: Parameter Out of Range
  Input: {"cmd": "set_volume", "volume": 150}
  ├─ Parse succeeds
  ├─ Extract volume = 150
  ├─ Clamp to 100 (if (volume > 100) volume = 100;)
  ├─ Apply: volume = 100
  └─ Response: {"status": "ok", "volume": 100}

Scenario 4: Unknown Command
  Input: {"cmd": "unknown_command"}
  ├─ Parse succeeds
  ├─ parseCommandString() → ConfigCommand::INVALID
  ├─ Switch statement: default case
  ├─ Log: "Unknown config command: unknown_command"
  └─ (Currently no response, could add one)
```

## 9. Integration Points

```
NetworkManager
  │
  ├─ Public API (New)
  │  ├─ sendDeviceHandshake()
  │  ├─ handleConfigCommand()
  │  ├─ applyVolumeConfig()
  │  ├─ applyBrightnessConfig()
  │  ├─ applyDeviceNameConfig()
  │  ├─ applyWiFiConfig()
  │  ├─ getCurrentStatusJson()
  │  ├─ onConfigUpdate()
  │  └─ setManagers()
  │
  ├─ Callbacks
  │  ├─ on_text_cb (existing - still used)
  │  ├─ on_binary_cb (existing - still used)
  │  ├─ on_disconnect_cb (existing - still used)
  │  └─ on_config_update_cb (new - for config changes)
  │
  └─ Manager References (optional)
     ├─ audio_manager → can call setVolume()
     └─ display_manager → can call setBrightness()
```

## 10. WebSocket Message Timeline

```
Time    Direction   Message Content
────────────────────────────────────────────────────────────────────
0ms     Device→S    CONNECT to ws://server:8000/ws
100ms   Server→D    Connection established
150ms   Device→S    {"cmd": "device_handshake", "device_id": "..."}
160ms   Server      [Parse handshake, store device_id in registry]
200ms   Server→D    {"cmd": "set_volume", "volume": 75}
220ms   Device      [Parse, validate, apply config]
250ms   Device→S    {"status": "ok", "volume": 75, "device_id": "..."}
260ms   Server      [Verify device_id, update record]
300ms   Server→D    {"cmd": "request_status"}
320ms   Device→S    {"status": "ok", "device_id": "...", "volume": 75, ...}
330ms   Server      [Update UI with full device status]
```

## 11. Data Flow Diagram

```
WebSocket Stream
      │
      ▼
Router (handleWsTextMessage)
      │
      ├─ Detect config command
      │  └─ handleConfigCommand()
      │     └─ Route to handler
      │
      └─ Route emotion codes
         └─ parseEmotionCode()
            └─ Set emotion state

Config Command Handler
      │
      ├─ Parse JSON (cJSON)
      ├─ Extract command type
      ├─ Validate parameters
      └─ Route to applier
         │
         ├─ applyVolumeConfig()
         ├─ applyBrightnessConfig()
         ├─ applyDeviceNameConfig()
         ├─ applyWiFiConfig()
         └─ etc.

Config Applier
      │
      ├─ Apply to hardware
      │  └─ audio_manager/display_manager (if set)
      │
      ├─ Build response JSON
      │  ├─ Set status
      │  ├─ Set relevant fields
      │  └─ Include device_id
      │
      ├─ Send response
      │  └─ ws->sendText()
      │
      └─ Callback
         └─ on_config_update_cb() (if registered)
```

---

## Legend

```
──────→  JSON message (Server → Device)
←────────  JSON response (Device → Server)
    │      Process flow
    ▼      Result/next step
    ✓      Success/verify
    ├      Decision point
    └      Alternative path
```

**Note**: These diagrams are for reference. For implementation details, see [CODE_CHANGES.md](CODE_CHANGES.md) and actual source code.
