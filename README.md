# PTalk - Trợ Lý Giọng Nói ESP32

Firmware modular, hướng sự kiện cho thiết bị trợ lý giọng nói dựa trên ESP32 với kết nối WiFi, xử lý âm thanh I/O, quản lý màn hình và tối ưu hóa năng lượng.

## 🎯 Tính Năng Chính

- **Voice Input/Output**: Ghi âm I2S (mic INMP441) và phát âm thanh (loa MAX98357)
- **Audio Codecs**: Hỗ trợ nén ADPCM và Opus
- **Display Management**: Driver màn hình ST7789 với hệ thống animation, render trực tiếp qua AnimationPlayer (không dùng framebuffer)
- **Network Connectivity**: Tích hợp WiFi và WebSocket client
- **Emotion System**: Hệ thống cảm xúc điều khiển từ server (WebSocket → `NetworkManager::parseEmotionCode()` → `StateManager` → Display)
- **Power Management**: Giám sát pin qua ADC, phát hiện sạc TP4056
- **State Management**: Event hub trung tâm với pattern publish-subscribe thread-safe
- **Multi-threaded**: Kiến trúc đa luồng FreeRTOS
- **Modular Design**: Tách biệt rõ ràng giữa hardware drivers và application logic
- **OTA Update**: Hỗ trợ cập nhật firmware 
- **WebSocket Configuration**: Cấu hình động qua WebSocket (xem docs/WEBSOCKET_CONFIG_*)

## 📋 Yêu Cầu Hệ Thống

- **Platform**: ESP32 (ESP-IDF framework)
- **Framework**: ESP-IDF với C++17
- **Build Tool**: PlatformIO
- **Monitor Speed**: 115200 baud
- **Flash Size**: 16MB (hỗ trợ OTA)

## 🏗️ Tổng Quan Kiến Trúc

PTalk tuân theo kiến trúc phân lớp, hướng sự kiện:

```
AppController (Bộ Điều Phối Trung Tâm)
        ↑
    StateManager (Event Hub - Thread-safe)
        ↑
   Managers (Business Logic)
   ├── AudioManager       - Quản lý capture/playback
   ├── DisplayManager     - Quản lý UI/animations (subscribe state trực tiếp)
   ├── NetworkManager     - WiFi + WebSocket + OTA streaming
   ├── PowerManager       - Giám sát pin, publish PowerState
   └── OTAUpdater         - Ghi/validate firmware chunks
        ↑
    Drivers (Hardware Abstraction)
   ├── I2SAudioInput_INMP441
   ├── I2SAudioOutput_MAX98357
   ├── DisplayDriver (ST7789)
   ├── TouchInput
   └── Power (ADC, GPIO)
        ↑
    Hardware (ESP32 peripherals)
```

### Core Components

| Component | Trách Nhiệm |
|-----------|-------------|
| **AppController** | Bộ điều phối trung tâm, xử lý AppEvent qua hàng đợi FreeRTOS, điều khiển control logic |
| **StateManager** | Event hub thread-safe với publish-subscribe pattern, quản lý tất cả state changes |
| **AudioManager** | Quản lý capture/playback audio, codec pipeline, stream buffer |
| **DisplayManager** | Điều khiển màn hình ST7789, animations, subscribe state để cập nhật UI tự động |
| **NetworkManager** | WiFi, WebSocket, retry/portal logic, OTA streaming |
| **PowerManager** | Giám sát ADC pin, smoothing %, publish PowerState |

## 📁 Cấu Trúc Dự Án

```
PTalk/
├── src/
│   ├── main.cpp                      # Entry point, khởi tạo AppController
│   ├── AppController.cpp/hpp         # Orchestrator chính, xử lý AppEvent
│   ├── Version.hpp                   # Metadata: APP_VERSION, DEVICE_MODEL, BUILD_DATE
│   ├── config/
│   │   ├── DeviceProfile.cpp/hpp     # Dependency injection, wiring managers/drivers
│   ├── assets/                       # Compiled C++ assets
│   │   ├── emotions/                 # Animation emotion (RLE-encoded)
│   │   │   ├── neutral.cpp/hpp
│   │   │   ├── idle.cpp/hpp
│   │   │   ├── listening.cpp/hpp
│   │   │   ├── happy.cpp/hpp
│   │   │   ├── sad.cpp/hpp
│   │   │   ├── thinking.cpp/hpp
│   │   │   └── stun.cpp/hpp
│   │   └── icons/                    # Icon assets (battery, charging, etc.)
│   ├── system/
│   │   ├── StateManager.cpp/hpp      # State hub (thread-safe)
│   │   ├── StateTypes.hpp            # State enumerations
│   │   ├── AudioManager.cpp/hpp      # Audio logic, codec tasks
│   │   ├── DisplayManager.cpp/hpp    # Display logic, animation playback
│   │   ├── NetworkManager.cpp/hpp    # Network logic, emotion parsing
│   │   ├── PowerManager.cpp/hpp      # Power monitoring
│   │   ├── BluetoothService.cpp/hpp  # Bluetooth support
│   │   └── OTAUpdater.cpp/hpp        # OTA firmware update
│   └── CMakeLists.txt
├── lib/
│   ├── audio/
│   │   ├── AudioCodec.hpp            # Abstract codec interface
│   │   ├── AudioInput.hpp/Output.hpp # Audio I/O abstractions
│   │   ├── I2SAudioInput_INMP441.cpp/hpp   # INMP441 mic driver
│   │   ├── I2SAudioOutput_MAX98357.cpp/hpp # MAX98357 speaker driver
│   │   ├── AdpcmCodec.cpp/hpp        # ADPCM compression
│   │   └── OpusCodec.cpp/hpp         # Opus compression
│   ├── display/
│   │   ├── DisplayDriver.cpp/hpp     # ST7789 low-level driver
│   │   ├── AnimationPlayer.cpp/hpp   # Multi-frame RLE animation engine
│   │   └── Font8x8.hpp               # Bitmap font data
│   ├── network/
│   │   ├── WifiService.cpp/hpp       # WiFi connectivity
│   │   ├── WebSocketClient.cpp/hpp   # WebSocket client
│   │   └── web_page.hpp              # Web UI assets (captive portal)
│   ├── power/
│   │   └── Power.cpp/hpp             # Power driver (ADC, GPIO)
│   └── touch/
│       └── TouchInput.cpp/hpp        # Touch/button input wrapper
├── include/
│   └── system/
│       └── WSConfig.hpp              # WebSocket config protocol
├── docs/
│   ├── ARCHITECTURE.md               # Kiến trúc chi tiết (đồng bộ với code)
│   ├── EMOTION_SYSTEM.md             # Tài liệu hệ thống cảm xúc
│   ├── WEBSOCKET_CONFIG_*.md         # Tài liệu cấu hình WebSocket
│   └── Software_Architecture.md      # Kiến trúc phần mềm tổng quan
├── scripts/
│   ├── convert_assets.py             # Convert images/GIFs thành C++ arrays
│   ├── convert_gif.py                # Convert GIF thành RLE animation
│   └── convert_logo.py               # Convert logo
├── server_test/
│   ├── dummy_server.py               # Server test WebSocket
│   └── dummy_server_cmd.py           # Server test command line
├── managed_components/               # ESP-IDF managed components
│   └── espressif__esp_websocket_client/
├── CMakeLists.txt                    # ESP-IDF build config
├── platformio.ini                    # PlatformIO config
├── partitions_16mb_ota.csv           # Partition table cho OTA
└── README.md
```

## 🔄 State Management (Quản Lý Trạng Thái)

Hệ thống sử dụng state machine tập trung thread-safe với các loại state sau:

### Interaction State
- `IDLE` - Hệ thống sẵn sàng, không hoạt động
- `TRIGGERED` - Phát hiện input (nút, wakeword, VAD)
- `LISTENING` - Đang ghi âm từ microphone
- `PROCESSING` - Đang chờ phản hồi từ server/AI
- `SPEAKING` - Đang phát âm thanh phản hồi
- `CANCELLING` - User hủy tương tác
- `MUTED` - Chế độ riêng tư (input disabled)
- `SLEEPING` - Chế độ tiết kiệm năng lượng

### Connectivity State
- `OFFLINE` - Không có kết nối WiFi
- `CONNECTING_WIFI` - Đang kết nối WiFi
- `WIFI_PORTAL` - Chế độ AP để cấu hình
- `CONNECTING_WS` - Đang kết nối WebSocket
- `ONLINE` - Kết nối đầy đủ

### Power State
- `NORMAL` - Pin đầy đủ
- `CHARGING` - Đang sạc
- `FULL_BATTERY` - Sạc đầy
- `CRITICAL` - Pin cực thấp (tự động deep sleep)
- `ERROR` - Lỗi pin/ngắt kết nối

### System State
- `BOOTING` - Khởi động
- `RUNNING` - Hoạt động bình thường
- `ERROR` - Lỗi hệ thống
- `MAINTENANCE` - Chế độ bảo trì
- `UPDATING_FIRMWARE` - Đang cập nhật OTA
- `FACTORY_RESETTING` - Đang reset về cài đặt gốc

### Emotion State
- `NEUTRAL` - Mặc định, không cảm xúc đặc biệt (Code: "00")
- `HAPPY` - Vui vẻ, thân thiện (Code: "01")
- `ANGRY` - Cảnh báo, khẩn cấp (Code: "02")
- `EXCITED` - Ngạc nhiên, phấn khích (Code: "03")
- `SAD` - Đồng cảm, quan tâm (Code: "10")
- `CONFUSED` - Không chắc chắn, cần làm rõ (Code: "12")
- `CALM` - Nhẹ nhàng, an tâm (Code: "13")
- `THINKING` - Đang xử lý (Code: "99")

## 🚀 Build và Flash

### Cài Đặt
```bash
# Cài PlatformIO
pip install platformio

# Cài ESP-IDF (nếu cần)
# https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/
```

### Build
```bash
# Build project
pio run -e esp32dev

# Build và monitor
pio run -e esp32dev -t monitor
```

### Upload
```bash
# Upload lên thiết bị
pio run -e esp32dev -t upload

# Upload và mở monitor
pio run -e esp32dev -t uploadandmonitor
```

## 🔧 Cấu Hình

Cấu hình chính trong `src/config/DeviceProfile.cpp`:
- **Pin assignments**: I2S, Display SPI, Touch, Power ADC
- **WiFi credentials**: SSID, password (hoặc dùng captive portal)
- **Audio buffer sizes**: Stream buffer sizes
- **Display parameters**: Resolution, rotation
- **Power thresholds**: Low battery, critical levels

## 📡 Yêu Cầu Phần Cứng

### Linh Kiện Bắt Buộc
- **MCU**: ESP32 DevKit (16MB flash khuyến nghị cho OTA)
- **Microphone**: INMP441 (I2S digital audio)
- **Speaker**: MAX98357 amplifier (I2S audio)
- **Display**: ST7789 1.3" LCD (240x240, SPI)
- **Battery**: Li-ion 3.7V + TP4056 charging module
- **WiFi**: Tích hợp sẵn ESP32 (2.4GHz)

### Pin Mapping
Cấu hình trong `DeviceProfile.cpp`:
- **I2S MIC (INMP441)**: BCLK, LRCLK, DIN
- **I2S Speaker (MAX98357)**: BCLK, LRCLK, DOUT
- **SPI Display (ST7789)**: MOSI, CLK, CS, DC, RST, BL
- **Power**: ADC pin cho battery voltage, GPIO cho TP4056 signals
- **Touch/Button**: GPIO cho user input

## 📚 Chi Tiết Modules

### Audio System
- **Input**: INMP441 digital microphone qua I2S
- **Output**: MAX98357 class-D amplifier qua I2S
- **Codecs**: ADPCM (bandwidth thấp) và Opus (chất lượng cao)
- **Streaming**: Real-time capture/playback với codec support
- **Tasks**: MicTask, CodecTask (core 0), SpkTask (core 1)

### Display System
- **Driver**: ST7789 SPI interface (240x240)
- **AnimationPlayer**: Direct rendering, không dùng framebuffer
- **Animations**: RLE-encoded sequences (xem `scripts/convert_assets.py`)
- **Registered emotions**: neutral, idle, listening, happy, sad, thinking, stun
- **Subscribe state**: DisplayManager tự động cập nhật UI khi state thay đổi

### Network System
- **WiFi**: ESP32 native 802.11b/g/n (2.4GHz)
- **WebSocket**: Persistent connection cho bidirectional communication
- **Captive Portal**: AP mode để provisioning WiFi
- **Retry Logic**: Tự động reconnect với backoff strategy
- **OTA Streaming**: Nhận firmware chunks qua WebSocket

### Emotion System
- **Flow**: Server gửi emotion code (2 chars) qua WebSocket → `NetworkManager::parseEmotionCode()` → `StateManager::setEmotionState()` → `DisplayManager` tự động play animation
- **Mapping**: Xem `docs/EMOTION_SYSTEM.md` để biết chi tiết codes
- **Thread-safe**: Callback được gọi ngoài lock để tránh deadlock

### Power System
- **Battery Monitoring**: ADC-based voltage measurement với smoothing
- **Charging Detection**: TP4056 CHRG/STDBY signals
- **Sleep Modes**: Light sleep và deep sleep (khi CRITICAL)
- **Hysteresis**: Smooth battery percentage reporting

## 🧵 Threading Model

### Task Configuration
| Task | Priority | Stack | Core | Ghi Chú |
|------|----------|-------|------|---------|
| AppControllerTask | 4 | 8KB | 1 | Main event loop |
| DisplayLoop | 3 | 6KB | 1 | UI/animation rendering |
| AudioSpkTask | 5 | 4KB | 1 | Speaker playback |
| AudioMicTask | 5 | 4KB | 0 | Microphone capture |
| AudioCodecTask | 4 | 8KB | 0 | Codec encode/decode |
| NetworkLoop | 3 | 8KB | NO_AFFINITY | WiFi + WebSocket |

### Core Assignment
- **Core 0**: WiFi driver, AudioMicTask, AudioCodecTask
- **Core 1**: AppControllerTask, DisplayLoop, AudioSpkTask

## 🔌 Event Flow

### State-driven (Reactive)
```
Hardware Change
    ↓
Driver detects
    ↓
Manager calls StateManager.setXxx()
    ↓
StateManager copies callbacks trong lock
    ↓
StateManager calls callbacks ngoài lock (thread-safe)
    ↓
AppController receives via queue (control logic)
DisplayManager/AudioManager receive trực tiếp (UI/audio)
```

### Event-driven (Deterministic)
```
User Action / System Event
    ↓
AppEvent posted to AppController::postEvent()
    ↓
AppControllerTask processes sequentially
    ↓
Map event → state/actions
    ↓
Update StateManager
    ↓
Propagate to subscribers
```

## 🔄 Lifecycle (Chu Trình Khởi Động)

### Khởi Động (app_main → AppController::start)
1. `AppController::init()` - Khởi tạo event queue
2. **Tạo AppControllerTask** (core 1, priority 4) - Đảm bảo queue ready
3. `PowerManager::start()` - Sample pin sớm
4. `DisplayManager::startLoop()` - Hiển thị boot UI
5. `NetworkManager::start()` - Kết nối WiFi/WebSocket
6. `AudioManager::start()` - Khởi động audio pipeline
7. `TouchInput::start()` - Kích hoạt input

### Tắt (AppController::stop)
Reverse order để tránh dangling references:
1. `NetworkManager::stop()`
2. `AudioManager::stop()`
3. `DisplayManager::stopLoop()`
4. `PowerManager::stop()`

## 📝 Implementation Notes

### Trạng Thái Hiện Tại
- ✅ **Audio pipeline**: Mic → Codec → Speaker tasks hoạt động tốt
- ✅ **Display system**: AnimationPlayer với RLE compression
- ✅ **Emotion parsing**: NetworkManager → StateManager → DisplayManager
- ✅ **State management**: Thread-safe publish-subscribe
- ✅ **Power management**: ADC monitoring, TP4056 detection
- ✅ **OTA Update**: OTAUpdater implemented, cần test integration
- ✅ **WebSocket config**: Dynamic configuration protocol (xem docs/)
- ⚠️ **NVS config**: Read/write helpers, cần UI để modify
- ⚠️ **Touch input**: Basic support, cần polish UX
- ⚠️ **Sleep/wake**: Logic implemented, cần test edge cases

### Code Style
- **C++17**: Modern C++ idioms
- **RAII**: Resource management
- **Smart pointers**: `std::unique_ptr` cho memory safety
- **FreeRTOS**: Task-based concurrency
- **Thread-safe**: Mutex + copy callbacks pattern
- **No raw new/delete**: Sử dụng smart pointers

## 🔧 Tool Scripts

### Convert Assets
```bash
# Convert icon
python scripts/convert_assets.py icon battery.png src/assets/icons/

# Convert emotion (GIF → RLE animation)
python scripts/convert_assets.py emotion happy.gif src/assets/emotions/ 20 true
# Args: type, input, output_dir, fps, loop
```

## 🐛 Known Issues & Fixes

- ✅ **Fixed**: Undefined TouchInput reference causing compilation errors
- ✅ **Fixed**: DisplayManager animation race conditions
- ⚠️ **Known**: Thỉnh thoảng WebSocket reconnect sau deep sleep cần thêm delay

## 📖 Tài Liệu Thêm

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) - Kiến trúc chi tiết đồng bộ với code
- [`docs/EMOTION_SYSTEM.md`](docs/EMOTION_SYSTEM.md) - Hệ thống cảm xúc và emotion codes
- [`docs/WEBSOCKET_CONFIG_*.md`](docs/) - Tài liệu cấu hình WebSocket

## 📄 License

[Thêm license tại đây]

## 👥 Contributors

**Trung Nguyen** - Core Developer

---

**Phiên Bản**: v1.0.4  
**Device Model**: PTalk-V1  
**Trạng Thái**: Development (đang phát triển)  
**Cập Nhật Cuối**: Tháng 1/2026
```
