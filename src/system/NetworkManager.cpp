#include "NetworkManager.hpp"
#include "WifiService.hpp"
#include "WebSocketClient.hpp"

#include "esp_log.h"

static const char* TAG = "NetworkManager";

NetworkManager::NetworkManager() = default;

NetworkManager::~NetworkManager() {
    stop();
}

// ============================================================================
// INIT
// ============================================================================
bool NetworkManager::init()
{
    ESP_LOGI(TAG, "Init NetworkManager");

    wifi = std::make_unique<WifiService>();
    ws   = std::make_unique<WebSocketClient>();

    if (!wifi || !ws) {
        ESP_LOGE(TAG, "Failed to allocate WifiService or WebSocketClient");
        return false;
    }

    wifi->init();
    ws->init();

    // --------------------------------------------------------------------
    // WiFi Status Callback
    // --------------------------------------------------------------------
    wifi->onStatus([this](int status){
        this->handleWifiStatus(status);
    });

    // --------------------------------------------------------------------
    // WebSocket Status Callback
    // --------------------------------------------------------------------
    ws->onStatus([this](int status){
        this->handleWsStatus(status);
    });

    // --------------------------------------------------------------------
    // WebSocket Message Callbacks
    // --------------------------------------------------------------------
    ws->onText([this](const std::string& msg){
        this->handleWsTextMessage(msg);
    });

    ws->onBinary([this](const uint8_t* data, size_t len){
        this->handleWsBinaryMessage(data, len);
    });

    ESP_LOGI(TAG, "NetworkManager init OK");
    return true;
}

// ============================================================================
// START / STOP
// ============================================================================
void NetworkManager::start()
{
    if (started) return;
    started = true;

    ESP_LOGI(TAG, "NetworkManager start()");

    wifi->autoConnect();

    publishState(state::ConnectivityState::CONNECTING_WIFI);
}

void NetworkManager::stop()
{
    if (!started) return;
    started = false;

    ESP_LOGW(TAG, "NetworkManager stop()");

    ws_should_run = false;
    ws_running = false;

    if (ws) ws->close();
    if (wifi) wifi->disconnect();
}

// ============================================================================
// UPDATE LOOP
// ============================================================================

void NetworkManager::update(uint32_t dt_ms)
{
    if (!started) return;

    tick_ms += dt_ms;

    // --------------------------------------------------------------------
    // Retry WebSocket nếu WiFi đã kết nối
    // --------------------------------------------------------------------
    if (ws_should_run && !ws_running)
    {
        if (ws_retry_timer > 0) {
            ws_retry_timer = (ws_retry_timer > dt_ms) ? (ws_retry_timer - dt_ms) : 0;
            return;
        }

        ESP_LOGI(TAG, "NetworkManager → Trying WebSocket connect...");
        publishState(state::ConnectivityState::CONNECTING_WS);

        ws->connect();

        ws_retry_timer = 2000;  // chống spam connect
    }
}

// ============================================================================
// SET CREDENTIALS
// ============================================================================
void NetworkManager::setCredentials(const std::string& ssid, const std::string& pass)
{
    if (wifi) {
        wifi->connectWithCredentials(ssid.c_str(), pass.c_str());
    }
}

// ============================================================================
// SEND MESSAGE TO WS
// ============================================================================
bool NetworkManager::sendText(const std::string& text)
{
    if (!ws_running) return false;
    return ws->sendText(text);
}

bool NetworkManager::sendBinary(const uint8_t* data, size_t len)
{
    if (!ws_running) return false;
    return ws->sendBinary(data, len);
}

// ============================================================================
// CALLBACK REGISTRATION
// ============================================================================
void NetworkManager::onServerText(std::function<void(const std::string&)> cb)
{
    on_text_cb = cb;
}

void NetworkManager::onServerBinary(std::function<void(const uint8_t*, size_t)> cb)
{
    on_binary_cb = cb;
}

// ============================================================================
// WIFI STATUS HANDLER
// ============================================================================
//
// WifiService status code:
//   0 = DISCONNECTED
//   1 = CONNECTING
//   2 = GOT_IP
//
void NetworkManager::handleWifiStatus(int status)
{
    switch (status)
    {
    case 0: // DISCONNECTED
        ESP_LOGW(TAG, "WiFi → DISCONNECTED");

        wifi_ready = false;

        ws_should_run = false;
        ws_running = false;
        ws->close();

        publishState(state::ConnectivityState::OFFLINE);
        break;

    case 1: // CONNECTING
        ESP_LOGI(TAG, "WiFi → CONNECTING");
        publishState(state::ConnectivityState::CONNECTING_WIFI);
        break;

    case 2: // GOT_IP
        ESP_LOGI(TAG, "WiFi → GOT_IP");

        wifi_ready = true;
        ws_should_run = true;
        ws_retry_timer = 10; // connect WS sớm nhất có thể

        publishState(state::ConnectivityState::CONNECTING_WS);
        break;
    }
}

// ============================================================================
// WEBSOCKET STATUS HANDLER
// ============================================================================
//
// WebSocketClient status code:
//   0 = CLOSED
//   1 = CONNECTING
//   2 = OPEN
//
void NetworkManager::handleWsStatus(int status)
{
    switch (status)
    {
    case 0: // CLOSED
        ESP_LOGW(TAG, "WS → CLOSED");
        ws_running = false;

        if (ws_should_run) {
            ws_retry_timer = 1500;  // retry nhẹ nhàng
            publishState(state::ConnectivityState::CONNECTING_WS);
        } else {
            publishState(state::ConnectivityState::OFFLINE);
        }
        break;

    case 1: // CONNECTING
        ESP_LOGI(TAG, "WS → CONNECTING");
        publishState(state::ConnectivityState::CONNECTING_WS);
        break;

    case 2: // OPEN
        ESP_LOGI(TAG, "WS → OPEN");
        ws_running = true;
        publishState(state::ConnectivityState::ONLINE);
        break;
    }
}

// ============================================================================
// MESSAGE FROM WEBSOCKET
// ============================================================================
void NetworkManager::handleWsTextMessage(const std::string& msg)
{
    ESP_LOGI(TAG, "WS Text Message: %s", msg.c_str());
    if (on_text_cb) on_text_cb(msg);
}

void NetworkManager::handleWsBinaryMessage(const uint8_t* data, size_t len)
{
    ESP_LOGI(TAG, "WS Binary Message (%zu bytes)", len);
    if (on_binary_cb) on_binary_cb(data, len);
}

// ============================================================================
// PUSH STATE TO STATEMANAGER
// ============================================================================
void NetworkManager::publishState(state::ConnectivityState s)
{
    StateManager::instance().setConnectivityState(s);
}
