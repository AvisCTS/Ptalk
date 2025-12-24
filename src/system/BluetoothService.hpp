#pragma once

#include <string>
#include <functional>
#include <vector>
#include <cstdint>

/**
 * BluetoothService (skeleton)
 * -----------------------------------------------------------
 * Placeholder service for future mobile-app provisioning over BLE.
 * - Not wired into main app yet.
 * - No actual BLE stack calls; only interface and state placeholders.
 */
class BluetoothService {
public:
    using ConfigHandler = std::function<void(const std::string& key, const std::string& value)>;
    using RawHandler    = std::function<void(const uint8_t* data, size_t len)>;

    BluetoothService();
    ~BluetoothService();

    // Initialize internal state (no BLE resources yet)
    bool init();

    // Start/stop placeholder session
    bool start();
    void stop();

    bool isRunning() const { return running_; }

    // Metadata
    void setDeviceName(const std::string& name);
    const std::string& deviceName() const { return device_name_; }

    // Handlers for future config messages (e.g., key/value pairs from app)
    void onConfig(ConfigHandler cb) { cfg_handler_ = std::move(cb); }
    void onRaw(RawHandler cb) { raw_handler_ = std::move(cb); }

    // Placeholder send APIs (no-op until BLE transport is implemented)
    bool sendText(const std::string& text);
    bool sendBinary(const std::vector<uint8_t>& data);

private:
    std::string device_name_ = "PTalk-BLE";
    ConfigHandler cfg_handler_{};
    RawHandler raw_handler_{};
    bool running_ = false;
};
