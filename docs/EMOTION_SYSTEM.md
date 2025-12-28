# PTalk Emotion System Documentation

## Overview

The PTalk Emotion System allows the device to display different emotional states during conversations. Emotions are determined by emotion codes sent from the server via WebSocket messages during the SPEAKING phase.

## Emotion Categories

The system supports **3 primary categories** and **8 total emotion states**:

### Primary Categories (3):
1. **NEUTRAL** (Natural) - Default, no special emotion
2. **HAPPY** (Happy) - Cheerful, friendly, positive response
3. **SAD** (Sad) - Empathetic, concerned, supportive response

### Extended Emotions (8 total):
1. **NEUTRAL** - Default expression (Code: "00")
2. **HAPPY** - Cheerful, friendly (Code: "01")
3. **ANGRY** - Alert, urgent (Code: "02")
4. **EXCITED** - Surprise, delight, enthusiasm (Code: "03")
5. **SAD** - Empathetic, concerned (Code: "10")  # NOTE: code is "10" in `NetworkManager::parseEmotionCode`
6. **CONFUSED** - Uncertain, seeking clarification (Code: "12")
7. **CALM** - Soothing, peaceful, reassuring (Code: "13")
8. **THINKING** - Processing state, internal use (Code: "99")

## Emotion Code Format

Server sends emotion codes as 2-character strings during SPEAKING state:

```
"00" → NEUTRAL
"01" → HAPPY
"02" → ANGRY
"03" → EXCITED
"10" → SAD
"12" → CONFUSED
"13" → CALM
"99" → THINKING
```

### Message Format from Server

**Simple format (direct emotion code):**
```
"01"
"10"
"00"
```

**Extended format (JSON):**
```json
{"emotion": "01", "chunk_id": 1, "timestamp": 123456}
```

Currently, the simple format is fully supported. Extended format parsing can be added via JSON parsing.

## Architecture

### State Management

The emotion state is managed through `StateManager`:

```cpp
// Set emotion state
StateManager::instance().setEmotionState(state::EmotionState::HAPPY);

// Get current emotion
state::EmotionState current = StateManager::instance().getEmotionState();

// Subscribe to emotion changes
int sub_id = StateManager::instance().subscribeEmotion(
    [](state::EmotionState emotion) {
        // Handle emotion change
        ESP_LOGI(TAG, "Emotion changed to: %d", (int)emotion);
    }
);
```

### Parsing Emotion Codes

NetworkManager provides `parseEmotionCode()` utility:

```cpp
// Convert WebSocket message to EmotionState
std::string code = "01";
state::EmotionState emotion = NetworkManager::parseEmotionCode(code);
// emotion == state::EmotionState::HAPPY
```

### Network Integration (Main Path)

During SPEAKING phase, the server sends emotion codes via WebSocket:

1. **WebSocketClient** receives message via `onText()` callback
2. **NetworkManager::handleWsTextMessage()** processes the message
3. If message is 2-char code → calls `parseEmotionCode()`
4. Result is passed to `StateManager::setEmotionState()`
5. **DisplayManager** subscribes and updates UI/animations

**Code Flow:**
```cpp
// In NetworkManager::handleWsTextMessage()
if (msg.length() == 2) {
    auto emotion = parseEmotionCode(msg);
    StateManager::instance().setEmotionState(emotion);
}
```

### Display Integration

**DisplayManager** responds to emotion changes:

```cpp
// Subscribe to emotion changes (in DisplayManager::init())
sub_emotion = StateManager::instance().subscribeEmotion(
    [this](state::EmotionState s) {
        this->handleEmotion(s);
    }
);

// Handle emotion → select animation
void DisplayManager::handleEmotion(state::EmotionState s) {
    switch (s) {
        case state::EmotionState::HAPPY:
            playEmotion("happy_animation");
            break;
        case state::EmotionState::SAD:
            playEmotion("sad_animation");
            break;
        // ... etc
    }
}
```

## Data Flow

```
WebSocket Server
       ↓
   Message "01"
       ↓
WebSocketClient::onText() callback
       ↓
NetworkManager::handleWsTextMessage()
       ↓
NetworkManager::parseEmotionCode("01") → HAPPY
       ↓
StateManager::setEmotionState(HAPPY)
       ↓
┌─────────────────────────┐
│  Emotion Subscribers:   │
├─────────────────────────┤
│ - DisplayManager        │ → Update animations
│ - AudioManager (opt)    │ → Adjust voice tone
│ - Custom handlers       │ → Any business logic
└─────────────────────────┘
```

## Usage Examples

### Example 1: Server sends emotion during TTS

```
1. InteractionState → SPEAKING
2. Server sends audio chunks
3. Server also sends emotion code "01"
4. DisplayManager shows "happy" animation while speaking
```

### Example 2: Multiple emotions during conversation

```
Server sends:
"01" → Happy, play happy animation
"10" → Sad, play sad animation
"00" → Neutral, play neutral animation

Note: current assets registered by `DeviceProfile` include: `neutral`, `idle`, `listening`, `happy`, `sad`, `thinking`, `stun` (used for CONFUSED). Animations for `angry`, `excited`, and `calm` are not yet implemented.
```

### Example 3: Emotion + other data (future)

```json
{"emotion": "01", "tone": "excited", "speed": 1.2}
```

Can be parsed with JSON parsing library (currently not implemented).

## Implementation Files

- **[src/system/StateTypes.hpp](../src/system/StateTypes.hpp)** - EmotionState enum definition
- **[src/system/StateManager.hpp](../src/system/StateManager.hpp)** - StateManager emotion methods
- **[src/system/StateManager.cpp](../src/system/StateManager.cpp)** - StateManager emotion implementation
- **[src/system/NetworkManager.hpp](../src/system/NetworkManager.hpp)** - parseEmotionCode() declaration, handleWsTextMessage()
- **[src/system/NetworkManager.cpp](../src/system/NetworkManager.cpp)** - parseEmotionCode() implementation, emotion handling (MAIN)
- **[src/AppController.hpp](../src/AppController.hpp)** - parseEmotionCode() wrapper for backward compatibility
- **[src/AppController.cpp](../src/AppController.cpp)** - parseEmotionCode() implementation (wrapper)
- **[src/system/DisplayManager.hpp](../src/system/DisplayManager.hpp)** - handleEmotion() declaration

## Next Steps

1. **Add missing emotion animations** - Create and register assets for `angry`, `excited`, and `calm` (DeviceProfile already registers several emotions).
2. **Add automated tests** - Unit tests for `parseEmotionCode()` and an integration test to send WS messages and assert StateManager and Display updates.
3. **Optional: AudioManager emotion handling** - Tune TTS parameters per emotion if desired.
4. **Optional: JSON parsing** - Add support for extended message format (e.g., {"emotion":"01","tone":"excited"}).
5. **Integration testing** - Verify full flow with server during SPEAKING phase and ensure fallback to NEUTRAL for unknown codes.

## API Quick Reference

```cpp
// StateManager
state::EmotionState getEmotionState();
void setEmotionState(state::EmotionState s);
int subscribeEmotion(EmotionCb cb);
void unsubscribeEmotion(int id);

// AppController
static state::EmotionState parseEmotionCode(const std::string& code);  // wrapper → NetworkManager::parseEmotionCode()

// DisplayManager
void handleEmotion(state::EmotionState s);
void registerEmotion(const std::string& name, const Animation& anim);
```
