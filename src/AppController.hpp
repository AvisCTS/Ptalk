#pragma once
#include <memory>
#include <atomic>
#include <cstdint>
#include "esp_sleep.h"
#include "system/StateManager.hpp"
#include "system/StateTypes.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// Optional External Messages / Intent / Commands (Key future extensibility)
namespace event
{
    enum class AppEvent : uint8_t
    {
        USER_BUTTON,             // UI physical press
        WAKEWORD_DETECTED,       // Wakeword engine triggers
        SERVER_FORCE_LISTEN,     // Remote control command
        OTA_BEGIN,               // Trigger OTA flow
        OTA_FINISHED,            // OTA process finished (success or fail)
        BATTERY_PERCENT_CHANGED, // Battery percentage changed (not a state)
        RELEASE_BUTTON,          // User requests to cancel current interaction
        SLEEP_REQUEST,           // Request to enter sleep mode
        CONFIG_DONE_RESTART,     // Configuration done, request restart
        WAKE_REQUEST             // Request to wake from sleep mode
    };
}

class NetworkManager; // WiFi + WebSocket
class AudioManager;   // Mic / Speaker
class DisplayManager; // UI / Animation
class PowerManager;   // Battery & power strategy
class TouchInput;     // Buttons or Touch Sensor
class OTAUpdater;     // Firmware update
// class ConfigManager;       // Configuration, NVS

// AppController is the central orchestrator: it owns the main task/queue,
// routes state changes via StateManager, handles AppEvents, and starts/stops
// Network/Audio/Display/Power/Touch/OTA modules. Singleton-managed.
class AppController {
public:
    struct Config {
        uint32_t deep_sleep_wakeup_sec = 30; // interval to re-check battery while in deep sleep
    };

    // Singleton accessor
    static AppController &instance();

    // ======= Lifecycle (consistent with all managers) =======
    // Initialize controller: create queue and subscribe to StateManager; call before start().
    bool init();

    // Start controller task then dependent managers (Power → Display → Network → Audio → Touch).
    void start();

    // Stop controller and all managers in reverse order; safe to call multiple times.
    void stop();

    // ======= External Actions =======
    void reboot();
    void enterSleep();
    void wake();
    void factoryReset();

    // Configure app-level parameters (e.g., deep sleep wake interval).
    void setConfig(const Config& cfg);

    // ======= Post application-level event to queue =======
    // Post an application event to the internal queue.
    void postEvent(event::AppEvent evt);

    // ======= Dependency injection =======
    // Attach owned module instances before init/start.
    void attachModules(std::unique_ptr<DisplayManager> displayIn,
                       std::unique_ptr<AudioManager> audioIn,
                       std::unique_ptr<NetworkManager> networkIn,
                       std::unique_ptr<PowerManager> powerIn,
                       std::unique_ptr<TouchInput> touchIn,
                       std::unique_ptr<OTAUpdater> otaIn);

    // ======= Module accessors (for testing) =======
    DisplayManager* getDisplay() const { return display.get(); }

    // ======= Emotion helpers =======
    // Parse emotion code from WebSocket message ("01" → HAPPY, "11" → SAD, etc.). Returns NEUTRAL if unknown.
    static state::EmotionState parseEmotionCode(const std::string& code);

private:
    AppController() = default;
    ~AppController();
    AppController(const AppController &) = delete;
    AppController &operator=(const AppController &) = delete;
    
    // ✅ Guard to prevent enterSleep() re-entrance
    std::atomic<bool> sleeping{false};

    // ======= Task Loop =======
    // Controller Task
    static void controllerTask(void *param);
    void processQueue();

    // ======= State callbacks =======
    void onInteractionStateChanged(state::InteractionState, state::InputSource);
    void onConnectivityStateChanged(state::ConnectivityState);
    void onSystemStateChanged(state::SystemState);
    void onPowerStateChanged(state::PowerState);

private:
    // ======= Subscription ID =======
    int sub_inter_id = -1;
    int sub_conn_id = -1;
    int sub_sys_id = -1;
    int sub_power_id = -1;

    // ======= Module pointers =======
    std::unique_ptr<NetworkManager> network;
    std::unique_ptr<AudioManager> audio;
    std::unique_ptr<DisplayManager> display;
    std::unique_ptr<PowerManager> power;
    // std::unique_ptr<ConfigManager> config;
    std::unique_ptr<OTAUpdater> ota;
    std::unique_ptr<TouchInput> touch;

    // ======= Task & Queue =======
    QueueHandle_t app_queue = nullptr;
    TaskHandle_t app_task = nullptr;

    // ======= Internal state =======
    std::atomic<bool> started{false};

    Config config_{};
};
