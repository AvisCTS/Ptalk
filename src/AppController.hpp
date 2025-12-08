#pragma once

#include <cstdint>
#include <string>

// Core modules
#include "system/StateManager.hpp"
// #include "Display.hpp"
// #include "DisplayAnimator.hpp"

// Added: Power Manager
#include "system/PowerManager.hpp"

// TODO: include network/audio when ready
// #include "WifiManager.hpp"
// #include "WebSocketClient.hpp"
// #include "AudioManager.hpp"

// TODO: include UI logic
// #include "UIStateController.hpp"

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/*
===============================================================================
  AppController – Central Orchestrator for the Entire System
===============================================================================

Responsibilities:
 - Create and initialize all modules
 - Start their FreeRTOS tasks on the correct cores
 - Bind StateManager updates → UI / Display / Power / Network
 - Apply system rules (boot sequence, idle policy, sleep policy)
 - Initialize and manage low-power mode transitions via PowerManager

Modules managed here:
 - Display + DisplayAnimator
 - PowerManager (new)
 - Network modules (WiFi + WS)
 - Audio subsystem (Mic, VAD, Speaker)
 - UI logic (UIStateController)
 - StateManager (central event bus)

===============================================================================
*/

class AppController {
public:
    AppController();
    ~AppController();

    // ========================================================================
    // SYSTEM INIT
    // ========================================================================
    
    // Initialize everything (called from app_main)
    bool begin();

    // ========================================================================
    // TASK STARTERS
    // ========================================================================

    // Display Animator task (core 1 recommended)
    void startDisplayAnimatorTask(
        UBaseType_t priority = 4,
        uint32_t stack = 4096,
        BaseType_t core = 1
    );

    // UI logic task (optional)
    void startUITask(
        UBaseType_t priority = 3,
        uint32_t stack = 4096,
        BaseType_t core = 1
    );

    // Network management task
    void startNetworkTask(
        UBaseType_t priority = 3,
        uint32_t stack = 4096,
        BaseType_t core = 0
    );

    // Audio processing (mic, encoder, vad, speaker)
    void startAudioTask(
        UBaseType_t priority = 4,
        uint32_t stack = 4096,
        BaseType_t core = 0
    );

    // Power management task (optional)
    void startPowerTask(
        UBaseType_t priority = 3,
        uint32_t stack = 4096,
        BaseType_t core = 0
    );

private:
    // ========================================================================
    // INTERNAL HELPERS
    // ========================================================================

    void loadConfig();
    void loadAnimations();
    void attachStateObservers();     // attach to PowerManager, Animator, UI

    // ========================================================================
    // MODULE INSTANCES
    // ========================================================================

    Display* display = nullptr;
    DisplayAnimator* animator = nullptr;

    // NEW: Power manager for sleep / brightness / power control
    PowerManager* power = nullptr;

    // TODO: UI
    // UIStateController* ui = nullptr;

    // TODO: Network
    // WifiManager* wifi = nullptr;
    // WebSocketClient* ws = nullptr;

    // TODO: Audio
    // AudioManager* audio = nullptr;

    // Global State Manager
    StateManager& state = StateManager::instance();

    // ========================================================================
    // TASK HANDLES
    // ========================================================================
    
    TaskHandle_t taskDisplayAnimator = nullptr;
    TaskHandle_t taskUI = nullptr;
    TaskHandle_t taskNetwork = nullptr;
    TaskHandle_t taskAudio = nullptr;
    TaskHandle_t taskPower = nullptr;   // NEW
};

