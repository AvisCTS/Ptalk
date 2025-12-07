#pragma once
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "driver/gpio.h"

class Power {
private:
    adc1_channel_t channel_;
    esp_adc_cal_characteristics_t adc_chars_;

    float R1_, R2_;

    float readVoltage();
    uint8_t voltageToPercent(float vbat);

public:
    Power(adc1_channel_t adc_channel, float R1, float R2);
    ~Power() = default;

    uint8_t getBatteryPercent();
    bool isCharging(gpio_num_t pin_chg = GPIO_NUM_NC);
    bool isFull(gpio_num_t pin_full = GPIO_NUM_NC);

};
