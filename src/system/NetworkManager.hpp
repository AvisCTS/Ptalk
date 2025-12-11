#pragma once

#include <memory>
#include <string>
#include <atomic>
#include <functional>

#include "system/StateTypes.hpp"
#include "system/StateManager.hpp"

// Forward declarations
#include "WifiService.hpp"
#include "WebSocketClient.hpp"
// class WifiService;
// class WebSocketClient;

/**
 * NetworkManager
 * ---------------------------------------
 * - Quản lý 2 lớp thấp: WifiService + WebSocketClient
 * - Không xử lý dữ liệu WS (AppController sẽ làm)
 * - Không chứa logic UI hoặc logic business
 * - Trả trạng thái về StateManager (ConnectivityState)
 *
 * ConnectivityState mapping:
 *   OFFLINE
 *   CONNECTING_WIFI
 *   WIFI_PORTAL
 *   CONNECTING_WS
 *   ONLINE
 */
class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    // Initialize WifiService + WebSocketClient
    bool init();

    // Start WiFi + WebSocket lifecycle
    void start();

    // Stop everything
    void stop();

    // Called periodically by AppController (20–50ms)
    void update(uint32_t dt_ms);

    // Set credentials (WiFi only)
    void setCredentials(const std::string& ssid, const std::string& pass);

    // Access raw modules (advanced integrations)
    WifiService* wifi() { return wifi_service.get(); }
    WebSocketClient* ws() { return ws_client.get(); }

private:
    // Handle WiFi callbacks (status_code from WifiService)
    void handleWifiStatus(int status);

    // Handle WS callbacks
    void handleWsStatus(int status);

    // StateManager helper
    void publishState(state::ConnectivityState s);

    // Internal: Start WS after WiFi connection
    void tryStartWebSocket();

private:
    // Modules
    std::unique_ptr<WifiService> wifi_service;
    std::unique_ptr<WebSocketClient> ws_client;

    // Internal flags
    bool started = false;
    bool ws_should_run = false;
    bool ws_running = false;

    // For timing / reconnection logic
    uint32_t tick_ms = 0;
    uint32_t ws_retry_timer = 0;
};
