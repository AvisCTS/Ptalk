#include "PowerManager.hpp"
#include "esp_log.h"

static const char* TAG = "PowerManager";

PowerManager::PowerManager(Power& power, uint8_t low_battery_threshold)
    : power_(power), low_batt_threshold_(low_battery_threshold)
{
}

PowerManager::~PowerManager()
{
    stopTask();
}

void PowerManager::startTask(uint32_t interval_ms)
{
    if (task_handle_) return;

    interval_tick_ = interval_ms / portTICK_PERIOD_MS;

    xTaskCreate(
        &PowerManager::taskEntry,
        "power_manager_task",
        4096,
        this,
        5,
        &task_handle_
    );

    ESP_LOGI(TAG, "PowerManager task started, interval=%d ms", interval_ms);
}

void PowerManager::stopTask()
{
    if (!task_handle_) return;

    vTaskDelete(task_handle_);
    task_handle_ = nullptr;
    ESP_LOGI(TAG, "PowerManager task stopped");
}

void PowerManager::taskEntry(void* param)
{
    PowerManager* self = static_cast<PowerManager*>(param);
    self->taskLoop();
}

void PowerManager::taskLoop()
{
    while(true)
    {
        evaluate();
        vTaskDelay(interval_tick_);
    }
}

void PowerManager::setState(state::PowerState new_state)
{
    if(new_state == current_state_) return;
    current_state_ = new_state;
    ESP_LOGI(TAG, "PowerState -> %d", static_cast<int>(new_state));
    StateManager::instance().setPowerState(new_state);
}

void PowerManager::evaluate()
{
    uint8_t percent = power_.getBatteryPercent();
    uint8_t charging = power_.isCharging();
    uint8_t full = power_.isFull();

    // PRIORITY DECISION
    if (full == 1) {
        setState(state::PowerState::FULL_BATTERY);
        return;
    }

    if (charging == 1) {
        setState(state::PowerState::CHARGING);
        return;
    }

    if (percent == BATTERY_INVALID) {
        setState(state::PowerState::LOW_BATTERY);
        return;
    }

    if (percent <= low_batt_threshold_) {
        setState(state::PowerState::LOW_BATTERY);
        return;
    }

    setState(state::PowerState::NORMAL);
}
