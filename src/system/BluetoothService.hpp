#pragma once

#include <string>
#include <functional>
#include <vector>
#include <cstdint>

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "Version.hpp"

struct WifiInfo;

// BLE GATT service used to provision basic device settings and Wi‑Fi credentials.
class BluetoothService
{
public:
    struct ConfigData
    {
        std::string device_name = "PTalk";
        uint8_t volume = 60;
        uint8_t brightness = 100;
        std::string ssid;
        std::string pass;
    };

    using OnConfigComplete = std::function<void(const ConfigData &)>;

    BluetoothService();
    ~BluetoothService();

    // Initialize BLE stack, cache networks for listing, and prepare GATT service; returns false on init failure.
    bool init(const std::string &adv_name, const std::vector<WifiInfo> &cached_networks = {});

    // Begin BLE advertising with the configured name; no-op if already started.
    bool start();

    // Stop advertising and keep configuration state intact.
    void stop();

    // Register callback triggered when the client sends the save command.
    void onConfigComplete(OnConfigComplete cb) { config_cb_ = cb; }

    static constexpr uint16_t SVC_UUID_CONFIG = 0xFF01;
    static constexpr uint16_t CHR_UUID_DEVICE_NAME = 0xFF02;
    static constexpr uint16_t CHR_UUID_VOLUME = 0xFF03;
    static constexpr uint16_t CHR_UUID_BRIGHTNESS = 0xFF04;
    static constexpr uint16_t CHR_UUID_WIFI_SSID = 0xFF05;
    static constexpr uint16_t CHR_UUID_WIFI_PASS = 0xFF06;
    static constexpr uint16_t CHR_UUID_APP_VERSION = 0xFF07;
    static constexpr uint16_t CHR_UUID_BUILD_INFO = 0xFF08;
    static constexpr uint16_t CHR_UUID_SAVE_CMD = 0xFF09;
    static constexpr uint16_t CHR_UUID_DEVICE_ID = 0xFF0A;
    static constexpr uint16_t CHR_UUID_WIFI_LIST = 0xFF0B;

private:
    static BluetoothService *s_instance;

    // GAP callback entrypoint; handles advertising events only.
    static void gapEventHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

    // GATT server callback entrypoint; dispatches create/add/read/write/MTU events.
    static void gattsEventHandler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

    // Process characteristic writes to build temporary config and trigger save.
    void handleWrite(esp_ble_gatts_cb_param_t *param);

    // Serve characteristic reads with the latest config, version, and Wi‑Fi list.
    void handleRead(esp_ble_gatts_cb_param_t *param, esp_gatt_if_t gatts_if);

private:
    std::string adv_name_;
    bool started_ = false;
    esp_gatt_if_t gatts_if_ = 0; // Stores GATT interface assigned at service creation
    uint16_t conn_id_ = 0xFFFF;
    uint16_t service_handle_ = 0;
    uint16_t char_handles[10] = {0}; // Handles for the 10 characteristics

    ConfigData temp_cfg_;
    OnConfigComplete config_cb_ = nullptr;

    std::string wifi_list_json_;
    std::string device_id_str_;
    std::vector<WifiInfo> wifi_networks_;  // Cached networks announced over BLE
    size_t wifi_read_index_ = 0;           // Streaming cursor for Wi‑Fi list reads
    uint16_t mtu_size_ = 23;               // Current BLE MTU (default 23, up to ~512)
};