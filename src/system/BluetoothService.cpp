#include "BluetoothService.hpp"

#include "esp_log.h"

static const char* TAG = "BluetoothService";

BluetoothService::BluetoothService() = default;
BluetoothService::~BluetoothService() = default;

bool BluetoothService::init()
{
    ESP_LOGI(TAG, "Init placeholder BluetoothService (BLE not yet wired)");
    return true;
}

bool BluetoothService::start()
{
    if (running_) return true;
    running_ = true;
    ESP_LOGI(TAG, "BluetoothService start (no-op)");
    return true;
}

void BluetoothService::stop()
{
    if (!running_) return;
    running_ = false;
    ESP_LOGI(TAG, "BluetoothService stop (no-op)");
}

void BluetoothService::setDeviceName(const std::string& name)
{
    device_name_ = name;
    ESP_LOGI(TAG, "Set device name: %s", device_name_.c_str());
}

bool BluetoothService::sendText(const std::string& text)
{
    ESP_LOGI(TAG, "sendText (placeholder): %s", text.c_str());
    // TODO: route via BLE transport when implemented
    return running_;
}

bool BluetoothService::sendBinary(const std::vector<uint8_t>& data)
{
    ESP_LOGI(TAG, "sendBinary (placeholder): %zu bytes", data.size());
    // TODO: route via BLE transport when implemented
    return running_;
}
