#include "NetworkManager.hpp"
#include "WifiService.hpp"
#include "WebSocketClient.hpp"
#include "esp_log.h"

static const char* TAG = "NetworkManager";

NetworkManager::NetworkManager() = default;

NetworkManager::~NetworkManager() {
    stop();
}

// ========================================================
// INIT
// ========================================================
bool NetworkManager::init()
{
    ESP_LOGI(TAG, "Init NetworkManager");

    wifi_service = std::make_unique<WifiService>();
    ws_client    = std::make_unique<WebSocketClient>();

    if (!wifi_service) {
        ESP_LOGE(TAG, "WifiService alloc failed");
        return false;
    }
    if (!ws_client) {
        ESP_LOGE(TAG, "WebSocketClient alloc failed");
        return false;
    }

    wifi_service->init();
    ws_client->init();

    // --------------------------
    // WiFi callback
    // --------------------------
    wifi_service->onStatus([this](int status_code){
        this->handleWifiStatus(status_code);
    });

    // --------------------------
    // WS callback
    // --------------------------
    ws_client->onStatus([this](int status_code){
        this->handleWsStatus(status_code);
    });

    ESP_LOGI(TAG, "NetworkManager init OK");
    return true;
}

// ========================================================
// START / STOP
// ========================================================
void NetworkManager::start()
{
    if (started) return;
    started = true;

    ESP_LOGI(TAG, "NetworkManager start()");

    // Start WiFi auto connect
    wifi_service->autoConnect();

    publishState(state::ConnectivityState::CONNECTING_WIFI);
}

void NetworkManager::stop()
{
    if (!started) return;
    started = false;

    ESP_LOGW(TAG, "NetworkManager stop()");

    if (ws_client) ws_client->close();
    if (wifi_service) wifi_service->disconnect();
}

// ========================================================
// Set credentials
// ========================================================
void NetworkManager::setCredentials(const std::string& ssid, const std::string& pass)
{
    if (wifi_service) {
        wifi_service->connectWithCredentials(ssid.c_str(), pass.c_str());
    }
}

// ========================================================
// MAIN UPDATE LOOP
// ========================================================
void NetworkManager::update(uint32_t dt_ms)
{
    if (!started) return;

    tick_ms += dt_ms;

    // ----------------------------------------------------
    // Try to start WS after WiFi ready
    // ----------------------------------------------------
    if (ws_should_run && !ws_running)
    {
        if (ws_retry_timer > 0) {
            ws_retry_timer = (ws_retry_timer > dt_ms) ? ws_retry_timer - dt_ms : 0;
            return;
        }

        ESP_LOGI(TAG, "Trying to connect WebSocket...");
        publishState(state::ConnectivityState::CONNECTING_WS);

        ws_client->connect();

        // Prevent spamming
        ws_retry_timer = 2000;
    }
}

// ========================================================
// HANDLE WIFI STATUS
// ========================================================
void NetworkManager::handleWifiStatus(int status)
{
    // status:
    // 0 = disconnected
    // 1 = connecting
    // 2 = got ip

    switch (status)
    {
    case 0: // DISCONNECTED
        ESP_LOGW(TAG, "WiFi → DISCONNECTED");

        ws_should_run = false;
        ws_running = false;
        if (ws_client) ws_client->close();

        publishState(state::ConnectivityState::OFFLINE);

        break;

    case 1: // CONNECTING
        ESP_LOGI(TAG, "WiFi → CONNECTING");
        publishState(state::ConnectivityState::CONNECTING_WIFI);
        break;

    case 2: // GOT_IP
        ESP_LOGI(TAG, "WiFi → GOT_IP");

        publishState(state::ConnectivityState::CONNECTING_WS);

        ws_should_run = true;
        ws_retry_timer = 10; // connect WS asap
        break;
    }
}

// ========================================================
// HANDLE WS STATUS
// ========================================================
void NetworkManager::handleWsStatus(int status)
{
    // WebSocket status:
    // 0 = CLOSED
    // 1 = CONNECTING
    // 2 = OPEN

    switch (status)
    {
    case 0: // CLOSED
        ESP_LOGW(TAG, "WS → CLOSED");
        ws_running = false;

        if (ws_should_run) {
            // Retry WS again later
            ws_retry_timer = 1500;
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
        ESP_LOGI(TAG, "WS → ONLINE");
        ws_running = true;
        publishState(state::ConnectivityState::ONLINE);
        break;
    }
}

// ========================================================
// HELPER: Push state to StateManager
// ========================================================
void NetworkManager::publishState(state::ConnectivityState s)
{
    StateManager::instance().setConnectivityState(s);
}
