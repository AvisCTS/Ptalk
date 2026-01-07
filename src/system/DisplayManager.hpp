#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>

#include "StateTypes.hpp"
#include "StateManager.hpp"
#include "AnimationPlayer.hpp"

// FreeRTOS (ESP32 task loop support)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


// Forward declarations
class DisplayDriver;       // ST7789 low-level driver
// Framebuffer removed - direct rendering architecture
class AnimationPlayer;     // Multi-frame animation engine

// ----------------------------------------------------------------------------
// Asset descriptors
// ----------------------------------------------------------------------------



//
// DisplayManager = UI Logic layer
// ----------------------------------------------------------------------------
// - Subscribes to StateManager when enabled
// - AppController can also call UI API directly
// - Handles emotion animation, icons, power-save mode
// - Uses DisplayDriver for actual drawing
//
class DisplayManager {
public:
   

    // Icon asset descriptor
    struct Icon {
        int w = 0;
        int h = 0;
        const uint8_t* rle_data = nullptr;
    };

    enum class IconPlacement {
        Custom,     // Use provided x,y
        Center,     // Centered on screen
        TopRight,   // Near top-right corner
        Fullscreen  // Origin (0,0), icon sized to screen
    };

public:
    DisplayManager();
    ~DisplayManager();

    // Initialize with low-level display driver (takes ownership) and set dimensions.
    bool init(std::unique_ptr<DisplayDriver> driver, int width = 240, int height = 320);

    // Real-time update; call every 20-50ms with elapsed milliseconds.
    void update(uint32_t dt_ms);

    // Lifecycle (consistent with other managers)
    bool startLoop(uint32_t interval_ms = 33,
                   UBaseType_t priority = 5,
                   uint32_t stackSize = 4096,
                   BaseType_t core = tskNO_AFFINITY);
    void stopLoop();
    bool isLoopRunning() const { return task_handle_ != nullptr; }
    void setUpdateIntervalMs(uint32_t interval_ms) { update_interval_ms_ = interval_ms; }
    
    // Aliases for consistency with other managers
    bool start(uint32_t interval_ms = 33, UBaseType_t priority = 5, uint32_t stackSize = 4096, BaseType_t core = tskNO_AFFINITY) {
        return startLoop(interval_ms, priority, stackSize, core);
    }
    void stop() { stopLoop(); }

    // Enable or disable automatic UI updates via StateManager subscriptions.
    void enableStateBinding(bool enable);

    // Exposed controls
    // Update battery percentage overlay (255 hides it).
    void setBatteryPercent(uint8_t p);
    

    // ======= OTA Update UI =======
    // Show OTA update screen and reset progress state.
    void showOTAUpdating();

    // Update OTA progress display (0-100%).
    void setOTAProgress(uint8_t current_percent);

    // Update OTA status message (e.g., "Downloading...", "Writing...").
    void setOTAStatus(const std::string& status);

    // Show OTA completed/success screen.
    void showOTACompleted();

    // Show OTA error screen with message.
    void showOTAError(const std::string& error_msg);

    // Show rebooting screen (after OTA success).
    void showRebooting();

    // Power saving mode (stop animations)
    void setPowerSaveMode(bool enable);

    // Backlight control passthrough
    void setBacklight(bool on);
    void setBrightness(uint8_t percent);

    // --- Asset Registration ---------------------------------------------------
    // Register an emotion animation by name (copied into registry).
    void registerEmotion(const std::string& name, const Animation1Bit& anim);

    // Register an icon asset by name (copied into registry).
    void registerIcon(const std::string& name, const Icon& icon);
    
    // --- Asset Playback (for testing/direct control) ---
    // Play a named emotion animation at coordinates (default centers animation).
    void playEmotion(const std::string& name, int x = 0, int y = 0);

    // Render a text message (centers when x or y < 0); stops animation while active.
    void playText(const std::string& text,
                  int x = -1,
                  int y = -1,
                  uint16_t color = 0xFFFF,
                  int scale = 1);

    // Clear any active text mode and resume animation.
    void clearText();

private:
    // Internal handlers mapping state -> UI behavior
    // Respond to interaction state changes (listening, thinking, etc.).
    void handleInteraction(state::InteractionState s, state::InputSource src);

    // Respond to connectivity state changes (WiFi/BLE/online).
    void handleConnectivity(state::ConnectivityState s);

    // Respond to system lifecycle state changes (booting, running, updating).
    void handleSystem(state::SystemState s);

    // Respond to power state changes (charging, critical, etc.).
    void handlePower(state::PowerState s);

    // Respond to emotion state changes to map to animations.
    void handleEmotion(state::EmotionState s);

    // Internal asset playback
    // Render an icon with optional placement helper.
    void playIcon(const std::string& name,
                  IconPlacement placement = IconPlacement::Custom,
                  int x = 0, int y = 0);
    static void taskEntry(void* arg);
    

private:
    std::unique_ptr<DisplayDriver> drv; // owned low-level driver
    // No Framebuffer - direct rendering to display!
    std::unique_ptr<AnimationPlayer> anim_player;

    // asset tables
    std::unordered_map<std::string, Animation1Bit> emotions;
    std::unordered_map<std::string, Icon> icons;

    // battery
    uint8_t battery_percent = 255;
    uint8_t prev_battery_percent = 255;  // Track previous value to avoid redraw

    // text playback state (mutually exclusive with animation)
    bool text_active_ = false;
    bool text_mode_cleared_ = false;  // Track if screen cleared for text mode
    std::string text_msg_{};
    int text_x_ = -1;
    int text_y_ = -1;
    uint16_t text_color_ = 0xFFFF;
    int text_scale_ = 1;

    // icon playback state (mutually exclusive with animation)
    bool icon_active_ = false;
    bool icon_mode_cleared_ = false;  // Track if screen cleared for icon mode

    // (toast system removed)

    // OTA update state
    uint8_t ota_progress_percent = 0;
    std::string ota_status_text = "";
    bool ota_updating = false;
    bool ota_completed = false;
    bool ota_error = false;
    std::string ota_error_msg = "";

    // subscriptions
    int sub_inter = -1;
    int sub_conn = -1;
    int sub_sys  = -1;
    int sub_power = -1;
    int sub_emotion = -1;

    bool binding_enabled = false;

    int width_ = 240;
    int height_ = 240;

    // Task loop state
    TaskHandle_t task_handle_ = nullptr;
    uint32_t update_interval_ms_ = 33; // ~30 FPS
    std::atomic<bool> task_running_{false};  // ✅ Graceful shutdown flag
};
