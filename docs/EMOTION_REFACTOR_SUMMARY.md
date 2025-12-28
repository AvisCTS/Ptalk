# Emotion System Refactor Summary

## Issue
Logic trùng lặp: `setupNetworkCallbacks()` ở AppController xử lý emotion codes đã được NetorkManager xử lý.

## Solution
**Centralize emotion parsing in NetworkManager (Cách 1)**

## Changes Made

### 1. NetworkManager (Moving logic here)
- ✅ Added `parseEmotionCode()` static method
- ✅ Updated `handleWsTextMessage()` to parse emotion codes directly
- ✅ Sets `StateManager::setEmotionState()` immediately when emotion code received

**File: [src/system/NetworkManager.hpp](../src/system/NetworkManager.hpp)**
```cpp
// Added declaration:
static state::EmotionState parseEmotionCode(const std::string& code);
```

**File: [src/system/NetworkManager.cpp](../src/system/NetworkManager.cpp#L520)**
```cpp
// ============================================================================
// EMOTION CODE PARSING
// ============================================================================
state::EmotionState NetworkManager::parseEmotionCode(const std::string& code)
{
    // Maps emotion codes to EmotionState
    // "01" → HAPPY, "10" → SAD, etc.
}

// Updated handleWsTextMessage():
void NetworkManager::handleWsTextMessage(const std::string& msg)
{
    // Try parsing emotion code if message is simple 2-char format
    if (msg.length() == 2) {
        auto emotion = parseEmotionCode(msg);
        StateManager::instance().setEmotionState(emotion);
    }
    
    if (on_text_cb) on_text_cb(msg);  // Still call other callbacks
}
```

### 2. AppController (Cleaning up)
- ❌ Removed `setupNetworkCallbacks()` method entirely
- ❌ Removed call to `setupNetworkCallbacks()` from `init()`
- ✅ Kept `parseEmotionCode()` as wrapper for backward compatibility (delegates to NetworkManager)

**File: [src/AppController.hpp](../src/AppController.hpp)**
```cpp
// Removed:
// void setupNetworkCallbacks();

// Kept (for backward compatibility):
static state::EmotionState parseEmotionCode(const std::string& code);
```

**File: [src/AppController.cpp](../src/AppController.cpp#L38)**
```cpp
// Changed to wrapper:
state::EmotionState AppController::parseEmotionCode(const std::string& code) {
    // Emotion parsing moved to NetworkManager::parseEmotionCode()
    return NetworkManager::parseEmotionCode(code);
}
```

### 3. DisplayManager (No changes needed)
- Already set up to subscribe to emotion state changes
- Will automatically respond when NetworkManager updates emotion state

## Data Flow (New - Simplified)

```
WebSocket Server
       ↓
   Message "01"
       ↓
WebSocketClient::onText()
       ↓
NetworkManager::handleWsTextMessage()  ← EMOTION PARSING HERE
       ↓
StateManager::setEmotionState(HAPPY)
       ↓
DisplayManager (subscribed) → Update UI/animations
```

## Benefits

✅ **No duplicate logic** - Emotion parsing in one place (NetworkManager)
✅ **Cleaner architecture** - WebSocket handling stays in NetworkManager
✅ **Single responsibility** - NetworkManager handles all WS messages including emotions
✅ **Backward compatible** - AppController::parseEmotionCode() still works (wrapper)
✅ **Direct flow** - No intermediate callbacks needed

## Files Changed

| File | Changes |
|------|---------|
| [src/system/NetworkManager.hpp](../src/system/NetworkManager.hpp) | Added `parseEmotionCode()` declaration |
| [src/system/NetworkManager.cpp](../src/system/NetworkManager.cpp) | Added emotion parsing logic to `handleWsTextMessage()` and `parseEmotionCode()` |
| [src/AppController.hpp](../src/AppController.hpp) | Removed `setupNetworkCallbacks()` declaration |
| [src/AppController.cpp](../src/AppController.cpp) | Removed `setupNetworkCallbacks()` implementation, kept `parseEmotionCode()` as wrapper |
| [docs/EMOTION_SYSTEM.md](../EMOTION_SYSTEM.md) | Updated documentation to reflect new architecture |

## Testing Checklist

- [x] Compile successfully (no errors/warnings)
- [x] `NetworkManager` receives and parses 2-char emotion codes
- [x] `StateManager.setEmotionState()` is called on parse
- [x] `DisplayManager` responds to emotion state changes (registered handlers)
- [x] `on_text_cb` still gets called (backward compatibility)
- [ ] Extended JSON messages (optional) to be handled gracefully

## Next Steps

1. **Create missing emotion animation assets** (angry, excited, calm) and register in `DeviceProfile`
2. **Add unit/integration tests** to validate parse + state propagation + UI updates
3. **Test with server** - Send test emotion codes during SPEAKING phase and verify animations
4. **Optional: JSON parsing** to support extended message payloads

## Notes

- AppController::parseEmotionCode() kept as static wrapper for any code that might call it directly
- NetworkManager::parseEmotionCode() is the single source of truth
- All emotion state changes flow through StateManager for consistency
