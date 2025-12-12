#pragma once
#include <memory>
#include <string>
#include <atomic>
#include <functional>

#include "system/StateTypes.hpp"
#include "system/StateManager.hpp"

class WifiService;         // Low-level WiFi
class WebSocketClient;     // Low-level WebSocket

/**
 * NetworkManager
 * -------------------------------------------------------------------
 * Nhiệm vụ:
 *  - Điều phối WiFi → WebSocket
 *  - Publish ConnectivityState lên StateManager
 *  - Cầu nối WebSocket ↔ AppController (nhận text/binary message)
 *  - Quyết định retry logic của WebSocket (không để trong WS driver)
 * 
 * Không làm:
 *  - Không scan wifi
 *  - Không xử lý portal HTML
 *  - Không chứa logic kết nối driver-level
 */
class NetworkManager
{
public:
    NetworkManager();
    ~NetworkManager();

    // ======================================================
    // INIT / START / STOP
    // ======================================================
    bool init();
    void start();
    void stop();

    // ======================================================
    // External API
    // ======================================================

    /// Cập nhật mỗi chu kỳ AppController loop
    void update(uint32_t dt_ms);

    /// Gán lại credentials khi user submit portal
    void setCredentials(const std::string& ssid, const std::string& pass);

    /// Gửi message lên server
    bool sendText(const std::string& text);
    bool sendBinary(const uint8_t* data, size_t len);

    /// Callback khi server gửi text message
    void onServerText(std::function<void(const std::string&)> cb);

    /// Callback khi server gửi binary
    void onServerBinary(std::function<void(const uint8_t*, size_t)> cb);

private:
    // ======================================================
    // Internal handlers
    // ======================================================
    void handleWifiStatus(int status_code);
    void handleWsStatus(int status_code);

    // Receive message from WebSocketClient
    void handleWsTextMessage(const std::string& msg);
    void handleWsBinaryMessage(const uint8_t* data, size_t len);

    // Push connectivity state lên StateManager
    void publishState(state::ConnectivityState s);

private:
    // ======================================================
    // Components
    // ======================================================
    std::unique_ptr<WifiService> wifi;
    std::unique_ptr<WebSocketClient> ws;

    // ======================================================
    // Runtime flags
    // ======================================================
    std::atomic<bool> started {false};

    // WiFi status flags
    bool wifi_ready = false;      // đã có IP hay chưa

    // WS runtime control
    bool ws_should_run = false;   // Manager muốn WS chạy
    bool ws_running    = false;   // WS thực sự open chưa

    // Retry timer (ms)
    uint32_t ws_retry_timer = 0;

    uint32_t tick_ms = 0;

    // ======================================================
    // App-level callbacks
    // ======================================================
    std::function<void(const std::string&)> on_text_cb = nullptr;
    std::function<void(const uint8_t*, size_t)> on_binary_cb = nullptr;
};
