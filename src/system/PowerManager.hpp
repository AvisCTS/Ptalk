#pragma once
#include <cstdint>
#include "../../lib/power/Power.hpp"
#include "StateManager.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class PowerManager {
public:
    PowerManager(Power& power, uint8_t low_battery_threshold = 20);
    ~PowerManager();

    // auto create task
    void startTask(uint32_t interval_ms = 2000);
    void stopTask();

private:
    Power& power_;
    uint8_t low_batt_threshold_;
    TaskHandle_t task_handle_ = nullptr;
    state::PowerState current_state_ = state::PowerState::NORMAL;
    uint32_t interval_tick_ = 2000 / portTICK_PERIOD_MS;

    void taskLoop();
    static void taskEntry(void* param);

    void setState(state::PowerState new_state);
    void evaluate();     // internal check logic
};
