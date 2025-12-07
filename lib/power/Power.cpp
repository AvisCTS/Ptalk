#include "Power.hpp"

Power::Power(adc1_channel_t adc_channel, float R1, float R2)
    : channel_(adc_channel), R1_(R1), R2_(R2)
{
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(channel_, ADC_ATTEN_DB_11);

    esp_adc_cal_characterize(
        ADC_UNIT_1,
        ADC_ATTEN_DB_11,
        ADC_WIDTH_BIT_12,
        1100,       // default vref
        &adc_chars_
    );
}

float Power::readVoltage(){
    uint32_t raw = adc1_get_raw(channel_);
    uint32_t mv = esp_adc_cal_raw_to_voltage(raw, &adc_chars_);

    float vadc = mv / 1000.0f;
    float vbat = vadc * ((R1_ + R2_) / R2_);

    return vbat;
}

uint8_t Power::voltageToPercent(float v){
    struct { float v; uint8_t p; } tbl[] = {
        {3.00, 0}, {3.30, 10}, {3.50, 25}, {3.70, 50},
        {3.90, 75}, {4.10, 90}, {4.20, 100}
    };

    uint8_t raw_percent = 0;

    if(v <= tbl[0].v) raw_percent = 0;
    else if(v >= tbl[6].v) raw_percent = 100;
    else {
        for(int i=0;i<6;i++){
            if(v >= tbl[i].v && v < tbl[i+1].v){
                float r = (v - tbl[i].v) / (tbl[i+1].v - tbl[i].v);
                raw_percent = tbl[i].p + r * (tbl[i+1].p - tbl[i].p);
                break;
            }
        }
    }


    static int8_t last_percent = -1;    // -1 = chưa có dữ liệu lúc đầu

    if(last_percent < 0){
        last_percent = raw_percent;     // Lần đầu luôn nhận
    }
    else if(abs(raw_percent - last_percent) >= 5){
        last_percent = raw_percent;     // Chỉ cập nhật khi lệch >=5%
    }

    return last_percent;
}


uint8_t Power::getBatteryPercent(){
    return voltageToPercent(readVoltage());
}

bool Power::isCharging(gpio_num_t pin){
    if(pin == GPIO_NUM_NC) return false;
    return gpio_get_level(pin) == 0;
}

bool Power::isFull(gpio_num_t pin){
    if(pin == GPIO_NUM_NC) return false;
    return gpio_get_level(pin) == 0;
}
