#include "AppController.hpp"
#include "system/NetworkManager.hpp"
#include "system/AudioManager.hpp"
#include "system/DisplayManager.hpp"
#include "system/PowerManager.hpp"
#include "../../lib/touch/TouchInput.hpp"
#include "system/OTAUpdater.hpp"

#include "esp_log.h"

#include <utility>

static const char *TAG = "AppController";

// ===================== Internal message type for queue =====================
struct AppMessage
{
    enum class Type : uint8_t
    {
        INTERACTION,
        CONNECTIVITY,
        SYSTEM,
        POWER,
        APP_EVENT
    } type;

    // Payloads (use only the field matching the type)
    state::InteractionState interaction_state;
    state::InputSource interaction_source;

    state::ConnectivityState connectivity_state;
    state::SystemState system_state;
    state::PowerState power_state;

    event::AppEvent app_event;
};

// ===================== Emotion parsing =====================

state::EmotionState AppController::parseEmotionCode(const std::string &code)
{
    // Emotion parsing moved to NetworkManager::parseEmotionCode()
    // This function kept for backward compatibility if needed
    return NetworkManager::parseEmotionCode(code);
}

// ===================== Singleton =====================

AppController &AppController::instance()
{
    static AppController inst;
    return inst;
}

// ===================== Lifecycle =====================

AppController::~AppController()
{
    stop();

    // Unsubscribe from StateManager to avoid callbacks after destruction
    auto &sm = StateManager::instance();
    if (sub_inter_id != -1)
        sm.unsubscribeInteraction(sub_inter_id);
    if (sub_conn_id != -1)
        sm.unsubscribeConnectivity(sub_conn_id);
    if (sub_sys_id != -1)
        sm.unsubscribeSystem(sub_sys_id);
    if (sub_power_id != -1)
        sm.unsubscribePower(sub_power_id);

    if (app_queue)
    {
        vQueueDelete(app_queue);
        app_queue = nullptr;
    }
}

void AppController::attachModules(std::unique_ptr<DisplayManager> displayIn,
                                  std::unique_ptr<AudioManager> audioIn,
                                  std::unique_ptr<NetworkManager> networkIn,
                                  std::unique_ptr<PowerManager> powerIn,
                                  std::unique_ptr<TouchInput> touchIn,
                                  std::unique_ptr<OTAUpdater> otaIn)
{
    if (started.load())
    {
        ESP_LOGW(TAG, "attachModules called after start; ignoring");
        return;
    }

    display = std::move(displayIn);
    audio = std::move(audioIn);
    network = std::move(networkIn);
    power = std::move(powerIn);
    touch = std::move(touchIn);
    ota = std::move(otaIn);
}

bool AppController::init()
{
    ESP_LOGI(TAG, "AppController init()");

    if (app_queue == nullptr)
    {
        app_queue = xQueueCreate(16, sizeof(AppMessage));
        if (!app_queue)
        {
            ESP_LOGE(TAG, "Failed to create app_queue");
            return false;
        }
    }

    if (!display)
        ESP_LOGW(TAG, "DisplayManager not attached");
    if (!audio)
        ESP_LOGW(TAG, "AudioManager not attached");
    if (!network)
        ESP_LOGW(TAG, "NetworkManager not attached");
    if (!power)
        ESP_LOGW(TAG, "PowerManager not attached");
    if (!touch)
        ESP_LOGW(TAG, "TouchInput not attached");
    if (!ota)
        ESP_LOGW(TAG, "OTAUpdater not attached");

    // Subscription architecture: AppController mediates state changes for control logic.
    // Other managers subscribe directly for UI/audio concerns. Benefits: avoids cross-cutting,
    // deterministic routing via a single queue, and is testable with mocked StateManager.

    auto &sm = StateManager::instance();

    sub_inter_id = sm.subscribeInteraction(
        [this](state::InteractionState s, state::InputSource src)
        {
            AppMessage msg{};
            msg.type = AppMessage::Type::INTERACTION;
            msg.interaction_state = s;
            msg.interaction_source = src;
            if (app_queue)
            {
                xQueueSend(app_queue, &msg, 0);
            }
        });

    sub_conn_id = sm.subscribeConnectivity(
        [this](state::ConnectivityState s)
        {
            AppMessage msg{};
            msg.type = AppMessage::Type::CONNECTIVITY;
            msg.connectivity_state = s;
            if (app_queue)
            {
                xQueueSend(app_queue, &msg, 0);
            }
        });

    sub_sys_id = sm.subscribeSystem(
        [this](state::SystemState s)
        {
            AppMessage msg{};
            msg.type = AppMessage::Type::SYSTEM;
            msg.system_state = s;
            if (app_queue)
            {
                xQueueSend(app_queue, &msg, 0);
            }
        });

    sub_power_id = sm.subscribePower(
        [this](state::PowerState s)
        {
            AppMessage msg{};
            msg.type = AppMessage::Type::POWER;
            msg.power_state = s;
            if (app_queue)
            {
                xQueueSend(app_queue, &msg, 0);
            }
        });

    return true;
}

void AppController::start()
{
    if (started.load())
    {
        ESP_LOGW(TAG, "AppController already started");
        return;
    }

    started.store(true);

    // 1️⃣ Start the main controller task FIRST
    BaseType_t res = xTaskCreatePinnedToCore(
        &AppController::controllerTask,
        "AppControllerTask",
        4096,
        this,
        4,
        &app_task,
        1
    );

    if (res != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create AppControllerTask");
        started.store(false);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    // 2️⃣ Start PowerManager
    if (power)
    {
        if (!power->init())
        {
            ESP_LOGE(TAG, "PowerManager init failed");
        }
        else
        {
            power->start();
            power->sampleNow();
        }
    }

    // 3️⃣ Start DisplayManager
    if (display)
    {
        if (!display->isLoopRunning() && !display->startLoop(33, 3, 4096, 1))
        {
            ESP_LOGE(TAG, "DisplayManager startLoop failed");
        }
    }

    // ============================================================================
    // 4️⃣ Start NetworkManager + Setup OTA callbacks
    // ============================================================================
    if (network)
    {
        // ✅ CRITICAL: Register OTA callback BEFORE starting network
        // This ensures handlers are ready when server sends REQUEST_OTA via MQTT
        network->onServerOTARequest([this]()
        {
            ESP_LOGI(TAG, "🔄 Server initiated OTA via MQTT - setting up handlers");
            
            // Set system state (for UI)
            StateManager::instance().setSystemState(state::SystemState::UPDATING_FIRMWARE);
            
            // ✅ Register chunk handler (called for each binary chunk)
            network->onFirmwareChunk([this](const uint8_t *data, size_t size)
            {
                if (!ota) {
                    ESP_LOGE(TAG, "OTA module not available!");
                    return;
                }

                // Begin OTA on first chunk
                if (!ota->isUpdating()) {
                    uint32_t expected_size = network->getFirmwareExpectedSize();
                    std::string expected_sha = network->getFirmwareExpectedChecksum();

                    ESP_LOGI(TAG, "📦 Beginning OTA: size=%u, sha256=%s", 
                             expected_size, expected_sha.c_str());

                    if (!ota->beginUpdate(expected_size, expected_sha)) {
                        ESP_LOGE(TAG, "❌ OTA begin failed!");
                        StateManager::instance().setSystemState(state::SystemState::ERROR);
                        return;
                    }
                }

                // Write chunk to flash
                int written = ota->writeChunk(data, size);
                if (written < 0) {
                    ESP_LOGE(TAG, "❌ OTA write failed, aborting");
                    ota->abortUpdate();
                    StateManager::instance().setSystemState(state::SystemState::ERROR);
                }
            });

            // ✅ Register complete handler (called when all chunks received)
            network->onFirmwareComplete([this](bool success, const std::string &msg)
            {
                if (success) {
                    ESP_LOGI(TAG, "✅ OTA transfer complete: %s", msg.c_str());
                    postEvent(event::AppEvent::OTA_FINISHED);
                } else {
                    ESP_LOGE(TAG, "❌ OTA failed: %s", msg.c_str());
                    StateManager::instance().setSystemState(state::SystemState::ERROR);
                }
            });
            
            ESP_LOGI(TAG, "✅ OTA handlers registered successfully");
        });

        // Check battery before starting network
        auto ps = StateManager::instance().getPowerState();
        if (ps == state::PowerState::CRITICAL)
        {
            ESP_LOGW(TAG, "Skipping NetworkManager start due to critical battery");
        }
        else
        {
            network->start();
        }
    }

    // 5️⃣ Start AudioManager
    if (audio)
    {
        auto ps = StateManager::instance().getPowerState();
        if (ps == state::PowerState::CRITICAL)
        {
            ESP_LOGW(TAG, "Skipping AudioManager start due to critical battery");
        }
        else
        {
            audio->start();
        }
    }

    // 6️⃣ Start TouchInput
    if (touch)
    {
        auto ps = StateManager::instance().getPowerState();
        if (ps == state::PowerState::CRITICAL)
        {
            ESP_LOGW(TAG, "Skipping TouchInput start due to critical battery");
        }
        else
        {
            touch->start();
        }
    }

    ESP_LOGI(TAG, "AppController started");
}

void AppController::stop()
{
    started.store(false);

    ESP_LOGI(TAG, "AppController stopping (reverse startup order)...");

    // Stop modules in REVERSE order of startup
    // Startup: PowerManager → DisplayManager → NetworkManager → AudioManager
    // Shutdown: AudioManager → NetworkManager → DisplayManager → PowerManager

    if (network)
    {
        network->stopPortal();
        network->stop();
        ESP_LOGD(TAG, "NetworkManager stopped");
    }

    if (audio)
    {
        audio->stop();
        ESP_LOGD(TAG, "AudioManager stopped");
    }

    if (display)
    {
        display->stopLoop();
        ESP_LOGD(TAG, "DisplayManager stopped");
    }

    if (power)
    {
        power->stop();
        ESP_LOGD(TAG, "PowerManager stopped");
    }

    // Wait for controllerTask to exit
    vTaskDelay(pdMS_TO_TICKS(100));
    if (app_task)
    {
        app_task = nullptr;
    }

    ESP_LOGI(TAG, "AppController stopped");
}

// ===================== External actions =====================

void AppController::reboot()
{
    ESP_LOGW(TAG, "System reboot requested");
    // TODO: Clean shutdown modules nếu cần
    esp_restart();
}

void AppController::enterSleep()
{
    // ✅ Guard against re-entrance
    if (sleeping.exchange(true))
    {
        ESP_LOGW(TAG, "enterSleep() already in progress");
        return;
    }

    ESP_LOGI(TAG, "Entering deep sleep due to critical battery");

    // Stop all modules before deep sleep
    if (network)
    {
        network->stopPortal();
        network->stop();
    }
    if (audio)
    {
        audio->stop();
    }
    if (display)
    {
        // Keep last frame visible briefly; turn off BL just before sleep
        display->stopLoop();
        // Delay to show last frame
        vTaskDelay(pdMS_TO_TICKS(5000));
        display->setBacklight(false);
    }

    // Wake up periodically to check battery
    const uint64_t wakeup_time_us = static_cast<uint64_t>(config_.deep_sleep_wakeup_sec) * 1000000ULL;
    esp_sleep_enable_timer_wakeup(wakeup_time_us);

    ESP_LOGI(TAG, "Configured to wake in %us to check battery", static_cast<unsigned>(config_.deep_sleep_wakeup_sec));
    esp_deep_sleep_start(); // DOES NOT RETURN
}

void AppController::wake()
{
    ESP_LOGI(TAG, "Wake requested");
    // TODO: xử lý wake logic (nếu dùng light sleep)
}

void AppController::factoryReset()
{
    ESP_LOGW(TAG, "Factory reset requested");
    // TODO:
    // 1) Xoá NVS config (config->factoryReset()?)
    // 2) set SystemState::FACTORY_RESETTING trong StateManager
    // 3) restart
}

void AppController::setConfig(const Config &cfg)
{
    config_ = cfg;
}

// ===================== Event Posting =====================

void AppController::postEvent(event::AppEvent evt)
{
    if (!app_queue)
        return;

    AppMessage msg{};
    msg.type = AppMessage::Type::APP_EVENT;
    msg.app_event = evt;
    xQueueSend(app_queue, &msg, 0);
}

// ===================== Task & Queue loop =====================

void AppController::controllerTask(void *param)
{
    auto *self = static_cast<AppController *>(param);
    self->processQueue();
}

void AppController::processQueue()
{
    ESP_LOGI(TAG, "AppController task started");

    AppMessage msg{};
    while (started.load())
    {
        if (xQueueReceive(app_queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            switch (msg.type)
            {
            case AppMessage::Type::INTERACTION:
                onInteractionStateChanged(msg.interaction_state, msg.interaction_source);
                break;
            case AppMessage::Type::CONNECTIVITY:
                onConnectivityStateChanged(msg.connectivity_state);
                break;
            case AppMessage::Type::SYSTEM:
                onSystemStateChanged(msg.system_state);
                break;
            case AppMessage::Type::POWER:
                onPowerStateChanged(msg.power_state);
                break;
            case AppMessage::Type::APP_EVENT:
                // Map AppEvent → state hoặc action
                switch (msg.app_event)
                {
                case event::AppEvent::USER_BUTTON:
                    // check ws is online
                    if (network)
                    {
                        auto conn_state = StateManager::instance().getConnectivityState();
                        if (conn_state != state::ConnectivityState::ONLINE)
                        {
                            ESP_LOGW(TAG, "Ignoring button press - not online");
                            break;
                        }
                    }
                    ESP_LOGI(TAG, "Button Pressed -> Start Listening");
                    // Interrupt any ongoing speaker output
                    if (audio && StateManager::instance().getInteractionState() == state::InteractionState::SPEAKING)
                    {
                        ESP_LOGI(TAG, "Interrupting speaker for button press");
                        audio->stopSpeaking();
                    }
                    // Chuyển thẳng sang LISTENING (hoặc TRIGGERED nếu muốn có tiếng Beep trước)
                    StateManager::instance().setInteractionState(
                        state::InteractionState::LISTENING,
                        state::InputSource::BUTTON);
                    break;
                case event::AppEvent::WAKEWORD_DETECTED:
                    StateManager::instance().setInteractionState(
                        state::InteractionState::TRIGGERED,
                        state::InputSource::WAKEWORD);
                    break;
                case event::AppEvent::SERVER_FORCE_LISTEN:
                    StateManager::instance().setInteractionState(
                        state::InteractionState::TRIGGERED,
                        state::InputSource::SERVER_COMMAND);
                    break;
                case event::AppEvent::SLEEP_REQUEST:
                    enterSleep();
                    break;
                case event::AppEvent::CONFIG_DONE_RESTART:
                    ESP_LOGI(TAG, "Configuration done - restarting system");
                    if (display)
                    {
                        display->playText("Config done. Restarting...", -1, -1, 0xFFFF, 1.5); // centered, white text
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }

                    esp_restart();
                    break;

                case event::AppEvent::WAKE_REQUEST:
                    wake();
                    break;
                case event::AppEvent::RELEASE_BUTTON:
                    if (network)
                    {
                        auto conn_state = StateManager::instance().getConnectivityState();
                        if (conn_state != state::ConnectivityState::ONLINE)
                        {
                            ESP_LOGW(TAG, "Ignoring button release - not online");
                            break;
                        }
                    }
                    StateManager::instance().setInteractionState(
                        state::InteractionState::IDLE,
                        state::InputSource::BUTTON);
                    break;
                case event::AppEvent::BATTERY_PERCENT_CHANGED:
                    // ✅ Removed: DisplayManager.update() queries power directly
                    break;
                 case event::AppEvent::OTA_BEGIN:
                //     // ✅ Only set state; DisplayManager subscribes and handles UI
                //     StateManager::instance().setSystemState(state::SystemState::UPDATING_FIRMWARE);

                //     if (network)
                //     {
                //         // Request firmware from server
                //         network->onFirmwareChunk([this](const uint8_t *data, size_t size)
                //                                  {
                //                     if (!ota) return;

                //                     if (!ota->isUpdating()) {
                //                         uint32_t expected_size = network->getFirmwareExpectedSize();
                //                         std::string expected_sha = network->getFirmwareExpectedChecksum();

                //                         if (!ota->beginUpdate(expected_size, expected_sha)) {
                //                             ESP_LOGE(TAG, "OTA begin failed (size=%u)", expected_size);
                //                             StateManager::instance().setSystemState(state::SystemState::ERROR);
                //                             return;
                //                         }
                //                     }

                //                     int written = ota->writeChunk(data, size);
                //                     if (written < 0) {
                //                         ESP_LOGE(TAG, "OTA write failed, aborting");
                //                         ota->abortUpdate();
                //                         StateManager::instance().setSystemState(state::SystemState::ERROR);
                //                     }
                //                     });

                //         network->onFirmwareComplete([this](bool success, const std::string &msg)
                //                                     {
                //                     if (success) {
                //                         postEvent(event::AppEvent::OTA_FINISHED);
                //                     } else {
                //                         StateManager::instance().setSystemState(state::SystemState::ERROR);
                //                     } });

                //         if (!network->requestFirmwareUpdate())
                //         {
                //             StateManager::instance().setSystemState(state::SystemState::ERROR);
                //         }
                //     }
                   break;
                case event::AppEvent::OTA_FINISHED:
                    if (ota && ota->isUpdating())
                    {
                        if (ota->finishUpdate())
                        {
                            // OTA success - reboot immediately
                            // Skip display update to avoid SPI conflict after flash operations
                            ESP_LOGI(TAG, "✅ OTA completed successfully! Rebooting in 1 second...");
                            vTaskDelay(pdMS_TO_TICKS(1000));
                            reboot();
                        }
                        else
                        {
                            ESP_LOGE(TAG, "OTA finishUpdate failed");
                            StateManager::instance().setSystemState(state::SystemState::ERROR);
                        }
                    }
                    else
                    {
                        ESP_LOGW(TAG, "OTA_FINISHED but no update in progress");
                        StateManager::instance().setSystemState(state::SystemState::ERROR);
                    }
                    break;
                }
                break;
            }
        }
    }

    ESP_LOGW(TAG, "AppController task stopping");
    vTaskDelete(nullptr);
}

// NetworkManager now owns its own update task
// ===================== State callbacks logic =====================
//
// IMPORTANT: These handlers execute in AppController task context (safe for queue operations)
// DisplayManager and AudioManager handle their own concerns in parallel via direct subscription
// AppController only handles cross-cutting control logic here
//
// NOTE: AppController no longer handles UI state changes.
// - InteractionState → AudioManager subscribes (controls audio)
// - ConnectivityState → DisplayManager subscribes (shows UI)
// - SystemState → DisplayManager subscribes (shows UI)
// - PowerState → DisplayManager subscribes (shows UI)

// Flow A: auto từ TRIGGERED → LISTENING
void AppController::onInteractionStateChanged(state::InteractionState s, state::InputSource src)
{
    ESP_LOGI(TAG, "Interaction changed: state=%d source=%d", (int)s, (int)src);

    auto &sm = StateManager::instance();

    // DisplayManager subscribes InteractionState directly and handles UI
    // AppController handles only control logic (audio/device commands)

    switch (s)
    {
    case state::InteractionState::TRIGGERED:
        // 🔁 Auto chuyển sang LISTENING
        sm.setInteractionState(state::InteractionState::LISTENING, src);
        break;

    case state::InteractionState::LISTENING:
        // Audio will auto-subscribe InteractionState changes
        break;

    case state::InteractionState::PROCESSING:
        // Pause capture (audio will handle via subscription)
        break;

    case state::InteractionState::SPEAKING:
        // Audio will handle via subscription
        break;

    case state::InteractionState::CANCELLING:
        // Sau khi cancel → đưa về IDLE
        sm.setInteractionState(state::InteractionState::IDLE, state::InputSource::UNKNOWN);
        break;

    case state::InteractionState::MUTED:
    case state::InteractionState::SLEEPING:
    case state::InteractionState::IDLE:
    default:
        break;
    }
}

void AppController::onConnectivityStateChanged(state::ConnectivityState s)
{
    ESP_LOGI(TAG, "Connectivity changed: %d", (int)s);

    // DisplayManager subscribes ConnectivityState directly and handles UI
    // AppController handles only control logic

    switch (s)
    {
    case state::ConnectivityState::OFFLINE:
        // When offline, immediately stop any listening/speaking
        if (audio)
        {
            audio->stopAll();
        }
        // Ensure interaction returns to IDLE to avoid background capture
        StateManager::instance().setInteractionState(
            state::InteractionState::IDLE,
            state::InputSource::UNKNOWN);
        break;

    case state::ConnectivityState::ONLINE:
        // No immediate audio action required; state-driven elsewhere
        break;
    case state::ConnectivityState::CONFIG_BLE:
        ESP_LOGW(TAG, "Config Mode: Cleaning up Audio to free ~72KB RAM...");

        // 1. Dừng Task và Xóa các StreamBuffer Audio (Giải phóng 72KB RAM)
        if (audio)
        {
            audio->stop();
            audio->freeResources(); // Bạn cần viết hàm này để delete StreamBuffers
        }

        // 2. Delay 1 chút để RAM kịp ổn định
        vTaskDelay(pdMS_TO_TICKS(1000));

        // 3. Bây giờ mới thực sự cho phép BLE khởi tạo
        if (network)
        {
            network->startBLEConfigMode();
        }
        break;
    case state::ConnectivityState::CONNECTING_WIFI:
        break;
    case state::ConnectivityState::WIFI_PORTAL:
        break;
    case state::ConnectivityState::CONNECTING_WS:
        break;
    default:
        break;
    }
}

void AppController::onSystemStateChanged(state::SystemState s)
{
    ESP_LOGI(TAG, "SystemState changed: %d", (int)s);

    // DisplayManager subscribes SystemState directly and handles UI
    // AppController handles only control logic

    switch (s)
    {
    case state::SystemState::ERROR:
        // Halt audio paths on system error
        if (audio)
        {
            audio->stopAll();
        }
        StateManager::instance().setInteractionState(
            state::InteractionState::IDLE,
            state::InputSource::UNKNOWN);
        break;

    case state::SystemState::UPDATING_FIRMWARE:
        // Ensure audio is stopped during OTA
        if (audio)
        {
            audio->stopAll();
        }
        StateManager::instance().setInteractionState(
            state::InteractionState::IDLE,
            state::InputSource::UNKNOWN);
        break;

    case state::SystemState::BOOTING:
    case state::SystemState::RUNNING:
    case state::SystemState::MAINTENANCE:
    case state::SystemState::FACTORY_RESETTING:
    default:
        break;
    }
}

void AppController::onPowerStateChanged(state::PowerState s)
{
    ESP_LOGI(TAG, "PowerState changed: %d", (int)s);

    switch (s)
    {
    case state::PowerState::NORMAL:
        if (audio)
        {
            // TODO: audio->setPowerSaving(false);
            audio->start();
        }
        // Khôi phục network nếu trước đó đã dừng
        if (network)
        {
            network->start();
        }
        if (touch)
        {
            touch->start();
        }
        break;

        // case state::PowerState::LOW_BATTERY:
        //     if (audio) {
        //         // TODO: audio->limitVolume();
        //     }
        //     // Dừng mọi task nặng (WS/Portal/STA)
        //     if (network) {
        //         network->stopPortal();
        //         network->stop();
        //     }
        //     break;

    case state::PowerState::CHARGING:
        break;

    case state::PowerState::FULL_BATTERY:
        if (network)
        {
            network->start();
        }
        break;

        // case state::PowerState::POWER_SAVING:
        //     if (audio) {
        //         // TODO: audio->setPowerSaving(true);
        //     }
        //     break;

    case state::PowerState::CRITICAL:
        if (audio)
        {
            audio->stop();
        }
        if (network)
        {
            network->stopPortal();
            network->stop();
        }
        if (touch)
        {
            touch->stop();
        }
        // ✅ Auto-sleep on critical battery
        ESP_LOGW(TAG, "Critical battery detected - entering deep sleep");
        enterSleep(); // Does not return
        break;

    case state::PowerState::ERROR:
        if (audio)
        {
            audio->stop();
        }
        break;
    }
}
