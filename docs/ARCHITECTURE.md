# PTalk System Architecture

## Overview
Enterprise-grade firmware architecture for PTalk device, emphasizing clean separation of concerns, thread safety, and scalability.

---

## 1. State Management (StateManager)

**Pattern:** Publish-Subscribe (thread-safe)

### Core States
- **InteractionState**: IDLE, TRIGGERED, LISTENING, PROCESSING, SPEAKING, CANCELLING, MUTED, SLEEPING
- **InputSource**: VAD, BUTTON, WAKEWORD, SERVER_COMMAND, SYSTEM, UNKNOWN (tracks what triggered the interaction)
- **ConnectivityState**: OFFLINE, CONNECTING_WIFI, WIFI_PORTAL, CONNECTING_WS, ONLINE
- **SystemState**: BOOTING, RUNNING, ERROR, MAINTENANCE, UPDATING_FIRMWARE, FACTORY_RESETTING
- **PowerState**: NORMAL, LOW_BATTERY, CHARGING, FULL_BATTERY, POWER_SAVING, CRITICAL, ERROR
- **EmotionState**: NEUTRAL, HAPPY, SAD, ANGRY, CONFUSED, EXCITED, CALM, THINKING (server-driven emotions)

### Key Properties
- ✅ **Thread-Safe**: Uses mutex + copy callbacks before notifying
- ✅ **No State Loops**: Callbacks copy before execution (prevents modification during iteration)
- ✅ **Subscription IDs**: Returns int ID for unsubscribe tracking
- ✅ **Input Source Tracking**: InteractionState callbacks include InputSource parameter

```cpp
// Interaction callback includes InputSource
auto id = StateManager::instance().subscribeInteraction(
    [this](state::InteractionState s, state::InputSource src) {
        // Handle state change with source context
    }
);
// Later...
StateManager::instance().unsubscribeInteraction(id);
```

---

## 2. Manager Lifecycle (Consistent Pattern)

All managers follow:
```cpp
bool init();      // Initialize resources (return bool for error checking)
void start();     // Begin operation (create tasks, enable timers)
void stop();      // Shutdown gracefully (reverse of start)
```

### Managers

#### **AppController**
- **Role**: Central mediator + event dispatcher
- **Responsibility**: 
  - Control logic (TRIGGERED → LISTENING transitions)
  - Cross-cutting concerns (power/network control)
  - Event routing (button, wakeword, OTA)
- **Dependencies**: All other managers (DI)
- **Subscription**: Queries StateManager directly (non-blocking queue)

#### **DisplayManager**
- **Role**: UI/Animation layer
- **Responsibility**: All visual output (UI updates, animations, toasts, emotion displays)
- **Subscriptions**: InteractionState, ConnectivityState, SystemState, PowerState, EmotionState
- **Key Point**: Subscribe directly; NO coupling to AppController for UI
- **Emotion System**: Displays emotion animations based on server emotion codes ("01"→HAPPY, "11"→SAD, etc.)

#### **AudioManager**
- **Role**: Audio I/O + state machine
- **Responsibility**: Microphone capture, speaker playback, audio codec management
- **Subscriptions**: InteractionState
- **Key Point**: Automatic state-driven (LISTENING/SPEAKING/IDLE)

#### **NetworkManager**
- **Role**: WiFi + WebSocket connectivity
- **Responsibility**: WiFi STA/AP modes, WebSocket client, OTA firmware download
- **Subscriptions**: None (explicit method calls from AppController)
- **Key Point**: No automatic state subscription (complex retry logic handled internally)

#### **PowerManager**
- **Role**: Battery monitoring + power strategy
- **Responsibility**: ADC sampling, state evaluation, power state publishing
- **Subscriptions**: None (publishes only)
- **Key Point**: Timer-based periodic sampling with exponential smoothing

---

## 3. Startup Sequence (Order Matters!)

```
main()
  └─ DeviceProfile::setup()
     ├─ Create managers
     ├─ Attach to AppController
     ├─ AppController::init()
     │  └─ Create queue
     │  └─ Subscribe state callbacks
     └─ AppController::start()
        ├─ 1️⃣ Create AppControllerTask FIRST (queue must be ready)
        │  └─ vTaskDelay(10ms) to ensure running
        ├─ 2️⃣ PowerManager::start() (monitor battery early)
        ├─ 3️⃣ DisplayManager::startLoop() (show portal status)
        └─ 4️⃣ NetworkManager::start() (conditional on battery)
```

**Critical:** AppControllerTask must be running BEFORE other modules trigger state changes.

---

## 4. Shutdown Sequence (Reverse Order)

```
AppController::stop()
  ├─ NetworkManager::stop()     (disable WiFi/WebSocket)
  ├─ AudioManager::stop()       (stop capture/playback)
  ├─ DisplayManager::stopLoop() (stop display task)
  ├─ PowerManager::stop()       (disable timer)
  └─ Unsubscribe from StateManager
```

**Important:** Reverse startup order to avoid dangling pointers.

---

## 5. Message Flow Architecture

### State-Driven Flow
```
[Hardware Event] 
  → PowerManager / TouchInput
  → StateManager.setPowerState()/setInteractionState()
  → Notify subscribers (in order):
     1. AppController (queue message)
     2. DisplayManager (direct call - UI updates)
     3. AudioManager (direct call - audio state machine)

[Server Emotion Code]
  → NetworkManager receives "01", "11", etc.
  → StateManager.setEmotionState(HAPPY/SAD/etc.)
  → DisplayManager renders emotion animation
```

### Event-Driven Flow
```
[User Action / Server Command]
  → AppEvent enum (USER_BUTTON, WAKEWORD_DETECTED, OTA_BEGIN, etc.)
  → AppController::postEvent()
  → Queue message
  → AppControllerTask processes (can access app_queue safely)
```

**Key Difference:**
- **State changes**: Reactive, all subscribers notified simultaneously
- **App events**: Sequential, single queue for deterministic ordering

---

## 6. Error Handling Strategy

### Pattern: Return bool + Log
```cpp
// ✅ Consistent
bool init() {
    if (!resource) {
        ESP_LOGE(TAG, "init: resource unavailable");
        return false;
    }
    return true;
}
```

### Error States (Optional)
Some PowerState/SystemState values for error recovery:
- `PowerState::ERROR` → Battery reading failed
- `SystemState::ERROR` → System error (audio/network failure)

### Logging Format
```cpp
ESP_LOGI(TAG, "StateType: new_value (param1:%.2f, param2:%d)", ...);
```

Example:
```cpp
ESP_LOGI(TAG, "PowerState: 2 (Volt:3.85V, %%:75, CHG:0, FULL:1)");
ESP_LOGI(TAG, "ConnectivityState: 4 (change)");
```

---

## 7. Task Configuration

| Task | Priority | Stack | Core | Purpose |
|------|----------|-------|------|---------|
| AppControllerTask | 4 | 4096B | 1 | State/event processing |
| DisplayLoop | 3 | 4096B | 1 | UI rendering (~30 FPS) |
| AudioMicTask | 5 | 4096B | 0 | Microphone capture |
| AudioCodecTask | 4 | 8192B | 0 | ADPCM/Opus codec processing |
| AudioSpkTask | 3 | 4096B | 1 | Speaker playback |
| NetworkLoop | 5 | 8192B | Any | WiFi/WebSocket management |
| wifi_retry | 5 | 4096B | Any | WiFi reconnection (temp) |
| PowerTimer | (timer) | - | - | Battery sampling |

**Notes:**
- AudioSpkTask on Core 1 for smooth playback without WiFi interference
- AudioCodecTask handles decode operations (ADPCM → PCM)
- NetworkLoop has no affinity (tskNO_AFFINITY) for flexibility

---

## 8. Dependency Injection Pattern

### Setup (DeviceProfile.cpp)
```cpp
auto display = std::make_unique<DisplayManager>();
auto audio = std::make_unique<AudioManager>();
auto network = std::make_unique<NetworkManager>();
auto power = std::make_unique<PowerManager>();
auto touch = std::make_unique<TouchInput>();
auto ota = std::make_unique<OTAUpdater>();

// Configure display with driver, framebuffer, animation player
display->attachDriver(std::move(driver), std::move(fb), std::move(anim));

// Configure audio with input, output, codec
audio->attachCodec(std::move(codec));
audio->attachInput(std::move(mic));
audio->attachOutput(std::move(spk));

// Attach all modules to AppController
app.attachModules(std::move(display), std::move(audio), 
                  std::move(network), std::move(power),
                  std::move(touch), std::move(ota));
```

**Benefits:**
- ✅ Managers don't create own dependencies
- ✅ Easy to mock for testing
- ✅ Clear ownership chain
- ✅ Flexible configuration per device profile

---

## 9. Thread Safety

### StateManager
- Mutex lock + copy callbacks before execution
- Prevents race conditions on subscription list

### Managers
- Each manager owns its task(s)
- Ring buffers for cross-task communication (AudioManager)
- No direct mutex sharing between managers (avoid deadlock)

### AppController
- Queue-based message passing (FreeRTOS queue is thread-safe)
- Safe to post from interrupt context

---

## 10. State Machine Example: Interaction

```
         ┌──────────────┐
         │    IDLE      │
         └──────┬───────┘
                │ USER_BUTTON / WAKEWORD_DETECTED / SERVER_FORCE_LISTEN
                ▼
         ┌──────────────┐
         │  TRIGGERED   │ ◄── Auto transition
         └──────┬───────┘
                │ (AppController TRIGGERED → LISTENING)
                ▼
         ┌──────────────┐
         │  LISTENING   │ ◄── AudioManager starts capture
         └──────┬───────┘
                │ (Server receives audio)
                ▼
         ┌──────────────┐
         │ PROCESSING   │ ◄── AudioManager pauses capture
         └──────┬───────┘
                │ (Server processes)
                ▼
         ┌──────────────┐
         │  SPEAKING    │ ◄── AudioManager starts playback
         └──────┬───────┘
                │ (Playback ends)
                ▼
         ┌──────────────┐
         │    IDLE      │ ◄── Auto return
         └──────────────┘
```

---

## 11. OTA Update Flow

```
AppEvent::OTA_BEGIN
  → AppController sets SystemState::UPDATING_FIRMWARE
  → DisplayManager shows OTA screen (via state subscription)
  → AppController requests firmware from NetworkManager
  → NetworkManager stream chunks to OTAUpdater
  → OTAUpdater writes chunks to flash
  → DisplayManager updates progress
  → OTAUpdater validates
  → AppEvent::OTA_FINISHED
  → Reboot
```

---

## 12. Battery Critical Flow

```
PowerManager samples voltage < CRITICAL_THRESHOLD
  → StateManager sets PowerState::CRITICAL
  → AppController::onPowerStateChanged() → enterSleep()
  → Stop network/audio
  → Set timer wakeup
  → esp_deep_sleep_start()
  ↓
  Wake (timer)
  → Boot sequence
  → PowerManager::sampleNow()
  → If still critical → sleep again
  → If recovered → continue boot
```

---

## 13. Scalability Considerations

✅ **Clean Separation**: Add new managers without touching existing code  
✅ **Pub/Sub**: Multiple subscribers without creating coupling  
✅ **Async Queue**: Prevent blocking on heavy operations  
✅ **Error Propagation**: Bool returns + logging for debugging  
✅ **Memory**: Fixed-size queues, pre-allocated resources  

---

## 14. Emotion System Architecture

### Server-Driven Emotion Display

**Flow:**
```
Server (during SPEAKING) sends emotion code
  → NetworkManager receives WebSocket message
  → Parse emotion code ("01" → HAPPY, "11" → SAD)
  → StateManager.setEmotionState(emotion)
  → DisplayManager subscription triggered
  → Load and play emotion animation
```

### Emotion Code Mapping

| Code | Emotion | Animation | Use Case |
|------|---------|-----------|----------|
| "00" | NEUTRAL | Default face | Normal responses |
| "01" | HAPPY | Cheerful smile | Positive, friendly responses |
| "02" | ANGRY | Alert expression | Urgent warnings |
| "03" | EXCITED | Surprised delight | Enthusiastic responses |
| "11" | SAD | Concerned look | Empathetic, supportive |
| "12" | CONFUSED | Uncertain face | Clarification needed |
| "13" | CALM | Peaceful expression | Soothing responses |
| "99" | THINKING | Processing animation | Internal processing |

### Implementation Details

**NetworkManager:**
- Parses emotion codes from WebSocket messages
- Static method `parseEmotionCode(code)` converts string → EmotionState
- Sets StateManager emotion during SPEAKING phase

**DisplayManager:**
- Subscribes to EmotionState changes
- Loads emotion animations from assets (happy.hpp, sad.hpp, etc.)
- Plays emotion animation while SPEAKING
- Returns to NEUTRAL after interaction ends

**Asset Requirements:**
- Emotion animations stored in `src/assets/emotions/`
- Converted using `scripts/convert_assets.py`
- Format: GIF → C++ header with frame data
- Currently implemented: HAPPY, SAD, THINKING

### References
- See [EMOTION_SYSTEM.md](EMOTION_SYSTEM.md) for detailed documentation
- See [EMOTION_REFACTOR_SUMMARY.md](EMOTION_REFACTOR_SUMMARY.md) for implementation history

---

## 15. Testing Strategy

```cpp
// Mock StateManager
MockStateManager sm;
AppController app;
app.init();

// Post event
app.postEvent(AppEvent::USER_BUTTON);

// Verify state change
EXPECT_EQ(sm.getInteractionState(), InteractionState::LISTENING);

// Verify AudioManager was called
EXPECT_CALL(mockAudio, startListening).Times(1);
```

---

## References

### Core Architecture
- **StateManager**: `src/system/StateManager.{hpp,cpp}`
- **StateTypes**: `src/system/StateTypes.hpp` (all state enums)
- **AppController**: `src/AppController.{hpp,cpp}`
- **Device Setup**: `src/config/DeviceProfile.{hpp,cpp}`

### Managers
- **DisplayManager**: `src/system/DisplayManager.{hpp,cpp}`
- **AudioManager**: `src/system/AudioManager.{hpp,cpp}`
- **NetworkManager**: `src/system/NetworkManager.{hpp,cpp}`
- **PowerManager**: `src/system/PowerManager.{hpp,cpp}`
- **OTAUpdater**: `src/system/OTAUpdater.{hpp,cpp}`

### Drivers & Libraries
- **Display**: `lib/display/` (DisplayDriver, Framebuffer, AnimationPlayer)
- **Audio**: `lib/audio/` (I2S input/output, ADPCM/Opus codecs)
- **Network**: `lib/network/` (WiFi, WebSocket)
- **Touch**: `lib/touch/TouchInput.{hpp,cpp}`
- **Power**: `lib/power/Power.{hpp,cpp}`

### Documentation
- **Emotion System**: `docs/EMOTION_SYSTEM.md`
- **Architecture**: `docs/ARCHITECTURE.md` (this file)
- **Software Architecture**: `docs/Software_Architecture.md`
