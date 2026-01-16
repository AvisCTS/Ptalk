#include "NetworkManager.hpp"
#include "WifiService.hpp"
#include "WebSocketClient.hpp"
#include "system/AudioManager.hpp"
#include "system/DisplayManager.hpp"
#include "system/PowerManager.hpp"
#include "system/WSConfig.hpp"

#include "esp_mac.h"
#include <sstream>
#include <iomanip>
#include "Version.hpp"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "esp_log.h"
#include "esp_timer.h"

std::string getDeviceMacID()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::stringstream ss;
    for (int i = 0; i < 6; ++i)
    {
        ss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (int)mac[i];
    }
    return ss.str();
}

static const char *TAG = "NetworkManager";

// Forward declarations for NVS storage utility functions
static bool nmgr_save_u8(const char* key, uint8_t value);
static bool nmgr_save_str(const char* key, const std::string& value);
static uint8_t nmgr_load_u8(const char* key, uint8_t def_val);
static std::string nmgr_load_str(const char* key, const char* def_val);

NetworkManager::NetworkManager() = default;

NetworkManager::~NetworkManager()
{
    stop();
}

// ============================================================================
// INIT
// ============================================================================
bool NetworkManager::init()
{
    ESP_LOGI(TAG, "Init NetworkManager");

    wifi = std::make_unique<WifiService>();
    ws = std::make_unique<WebSocketClient>();

    if (!wifi || !ws)
    {
        ESP_LOGE(TAG, "Failed to allocate WifiService or WebSocketClient");
        return false;
    }

    wifi->init();
    ws->init();

    // Apply configured WS URL if provided
    if (!config_.ws_url.empty())
    {
        ws->setUrl(config_.ws_url);
    }

    // --------------------------------------------------------------------
    // WiFi Status Callback
    // --------------------------------------------------------------------
    wifi->onStatus([this](int status)
                   { this->handleWifiStatus(status); });

    // --------------------------------------------------------------------
    // WebSocket Status Callback
    // --------------------------------------------------------------------
    ws->onStatus([this](int status)
                 { this->handleWsStatus(status); });

    // --------------------------------------------------------------------
    // WebSocket Message Callbacks
    // --------------------------------------------------------------------
    ws->onText([this](const std::string &msg)
               { this->handleWsTextMessage(msg); });

    ws->onBinary([this](const uint8_t *data, size_t len)
                 { this->handleWsBinaryMessage(data, len); });

    // Subscribe to interaction state updates
    sub_interaction_id = StateManager::instance().subscribeInteraction(
        [this](state::InteractionState s, state::InputSource src)
        {
            this->handleInteractionState(s);
        });

    ESP_LOGI(TAG, "NetworkManager init OK");
    return true;
}

// Overload: init with configuration
bool NetworkManager::init(const Config &cfg)
{
    config_ = cfg;
    return init();
}

// ============================================================================
// START / STOP
// ============================================================================
void NetworkManager::start()
{
    if (started)
        return;
    started = true;

    ESP_LOGI(TAG, "NetworkManager start()");

    // Prefer explicit credentials if provided in config
    if (!config_.sta_ssid.empty() && !config_.sta_pass.empty())
    {
        wifi->connectWithCredentials(config_.sta_ssid.c_str(), config_.sta_pass.c_str());
        publishState(state::ConnectivityState::CONNECTING_WIFI);
        // Spawn retry task to open portal if connection fails
        if (wifi_retry_task == nullptr)
        {
            xTaskCreate(&NetworkManager::retryWifiTaskEntry, "wifi_retry", 4096, this, 5, &wifi_retry_task);
        }
    }
    else
    {
        // Try auto-connect (saved creds). If available, still spawn retry task for fallback.
        wifi->autoConnect();
        publishState(state::ConnectivityState::CONNECTING_WIFI);

        // Always spawn retry task - even with autoConnect, if WiFi fails to actually connect,
        // we need the portal fallback after 5 seconds
        if (wifi_retry_task == nullptr)
        {
            ESP_LOGI(TAG, "Spawning WiFi retry task for fallback to portal if connection fails");
            xTaskCreate(&NetworkManager::retryWifiTaskEntry, "wifi_retry", 4096, this, 5, &wifi_retry_task);
        }
    }

    // Spawn internal update task so callers don't need to tick manually
    if (task_handle == nullptr)
    {
        BaseType_t rc = xTaskCreatePinnedToCore(
            &NetworkManager::taskEntry,
            "NetworkLoop",
            8192, // Increased from 4096 to prevent stack overflow
            this,
            5,
            &task_handle,
            tskNO_AFFINITY);
        if (rc != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create NetworkLoop task (%d)", (int)rc);
            task_handle = nullptr;
        }
    }
}

void NetworkManager::stop()
{
    if (!started)
        return;
    started = false;

    ESP_LOGW(TAG, "NetworkManager stop()");

    ws_should_run = false;
    ws_running = false;

    if (ws)
        ws->close();
    if (wifi)
        wifi->disconnect();

    if (task_handle)
    {
        TaskHandle_t th = task_handle;
        task_handle = nullptr;
        vTaskDelete(th);
    }
}

// ============================================================================
// UPDATE LOOP
// ============================================================================

void NetworkManager::update(uint32_t dt_ms)
{
    if (!started)
        return;

    tick_ms += dt_ms;
    if (ws_running && !ws->isConnected())
    {
        ws->close();
    }
    // --------------------------------------------------------------------
    // Retry WebSocket when Wi‑Fi is connected
    // --------------------------------------------------------------------
    if (ws_should_run && !ws_running)
    {
        if (ws_retry_timer > 0)
        {
            ws_retry_timer = (ws_retry_timer > dt_ms) ? (ws_retry_timer - dt_ms) : 0;
            return;
        }

        ESP_LOGI(TAG, "NetworkManager → Trying WebSocket connect...");
        publishState(state::ConnectivityState::CONNECTING_WS);

        // Ensure WS URL is configured before connecting
        if (!config_.ws_url.empty())
        {
            ws->setUrl(config_.ws_url);
        }
        ws->connect();

        ws_retry_timer = 5000; // 5 second delay between reconnect attempts
    }
}

void NetworkManager::taskEntry(void *arg)
{
    auto *self = static_cast<NetworkManager *>(arg);
    if (!self)
    {
        vTaskDelete(nullptr);
        return;
    }

    TickType_t prev = xTaskGetTickCount();
    for (;;)
    {
        if (!self->started.load())
        {
            // Clear our own handle before self-deleting
            self->task_handle = nullptr;
            vTaskDelete(nullptr);
        }

        TickType_t now = xTaskGetTickCount();
        uint32_t dt_ms = (now - prev) * portTICK_PERIOD_MS;
        prev = now;

        self->update(dt_ms ? dt_ms : self->update_interval_ms);

        vTaskDelay(pdMS_TO_TICKS(self->update_interval_ms));
    }
}

// ============================================================================
// SET CREDENTIALS
// ============================================================================
void NetworkManager::setCredentials(const std::string &ssid, const std::string &pass)
{
    if (wifi)
    {
        wifi->connectWithCredentials(ssid.c_str(), pass.c_str());
    }
}

// ============================================================================
// SEND MESSAGE TO WS
// ============================================================================
bool NetworkManager::sendText(const std::string &text)
{
    if (!ws_running)
        return false;
    return ws->sendText(text);
}

bool NetworkManager::sendBinary(const uint8_t *data, size_t len)
{
    if (!ws_running)
        return false;
    return ws->sendBinary(data, len);
}

// ============================================================================
// CALLBACK REGISTRATION
// ============================================================================
void NetworkManager::onServerText(std::function<void(const std::string &)> cb)
{
    on_text_cb = cb;
}

void NetworkManager::onServerBinary(std::function<void(const uint8_t *, size_t)> cb)
{
    on_binary_cb = cb;
}

void NetworkManager::onDisconnect(std::function<void()> cb)
{
    on_disconnect_cb = cb;
}

void NetworkManager::setWSImmuneMode(bool immune)
{
    ws_immune_mode = immune;
    if (immune)
    {
        ESP_LOGI(TAG, "WS immune mode ENABLED - WS will ignore WiFi fluctuations");
    }
    else
    {
        ESP_LOGI(TAG, "WS immune mode DISABLED - normal WS behavior");
    }
}

// ============================================================================
// RUNTIME CONFIG SETTERS
// ============================================================================
void NetworkManager::setWsUrl(const std::string &url)
{
    config_.ws_url = url;
    if (ws && !url.empty())
    {
        ws->setUrl(url);
    }
}

void NetworkManager::setApSsid(const std::string &apSsid)
{
    config_.ap_ssid = apSsid;
}

void NetworkManager::setDeviceLimit(uint8_t maxClients)
{
    config_.ap_max_clients = maxClients;
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
    ESP_LOGI(TAG, "handleWifiStatus called with status=%d", status);

    switch (status)
    {
    case 0: // DISCONNECTED
        ESP_LOGW(TAG, "WiFi → DISCONNECTED");

        wifi_ready = false;

        // Only close WS if NOT in immune mode (during audio streaming, keep WS alive)
        if (!ws_immune_mode)
        {
            ws_should_run = false;
            ws_running = false;
            if (ws)
                ws->close();
            publishState(state::ConnectivityState::OFFLINE);
        }
        else
        {
            ESP_LOGI(TAG, "WS immune mode active - ignoring WiFi disconnect, keeping WS alive");
            // Keep ws_should_run and ws_running unchanged
            // WS will survive temporary WiFi fluctuations
        }
        break;

    case 1: // CONNECTING
        ESP_LOGI(TAG, "WiFi → CONNECTING");

        publishState(state::ConnectivityState::CONNECTING_WIFI);
        break;

    case 2: // GOT_IP
        ESP_LOGI(TAG, "WiFi → GOT_IP");

        if (wifi_retry_task)
        {
            vTaskDelete(wifi_retry_task);
            wifi_retry_task = nullptr;
        }
        wifi_ready = true;
        ws_should_run = true;
        ws_retry_timer = 500; // Wait 500ms for WiFi to stabilize before WS connect

        publishState(state::ConnectivityState::CONNECTING_WS);
        break;
    }

    ESP_LOGI(TAG, "handleWifiStatus completed");
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

        if (on_disconnect_cb)
            on_disconnect_cb();

        if (wifi_ready)
        {
            ws_should_run = true;
            ws_retry_timer = 1500;
            publishState(state::ConnectivityState::CONNECTING_WS);
        }
        else
        {
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
        
        // Send device handshake to server with all info and device_id for linking
        sendDeviceHandshake();
        
        break;
    }
}

// ============================================================================
// MESSAGE FROM WEBSOCKET
// ============================================================================
void NetworkManager::handleWsTextMessage(const std::string &msg)
{
    ESP_LOGI(TAG, "WS Text Message: %s", msg.c_str());

    // Check for OTA_COMPLETE message
    if (msg == "OTA_COMPLETE")
    {
        ESP_LOGI(TAG, "🔧 OTA_COMPLETE received, calling firmware complete callback");
        firmware_download_active = false;
        if (on_firmware_complete_cb)
        {
            on_firmware_complete_cb(true, "OTA transfer complete");
        }
        return;
    }

    // Check if this is a config command (JSON with "cmd" field)
    cJSON *json = cJSON_Parse(msg.c_str());
    if (json)
    {
        cJSON *cmd_field = cJSON_GetObjectItem(json, "cmd");
        if (cmd_field && cmd_field->valuestring)  // Check if it's a string
        {
            // This is a config command
            cJSON_Delete(json);
            handleConfigCommand(msg);
            return;
        }
        cJSON_Delete(json);
    }

    // Try parsing emotion code if message is simple 2-char format
    if (msg.length() == 2)
    {
        auto emotion = parseEmotionCode(msg);
        StateManager::instance().setEmotionState(emotion);
        ESP_LOGI(TAG, "Emotion code: %s → %d", msg.c_str(), (int)emotion);
        // Don't return - still call on_text_cb for logging/debugging
    }

    if (on_text_cb)
        on_text_cb(msg);
}

void NetworkManager::handleWsBinaryMessage(const uint8_t *data, size_t len)
{
    // ESP_LOGI(TAG, "WS Binary Message (%zu bytes)", len);

    // Check if this is firmware data during OTA download
    if (firmware_download_active)
    {
        firmware_bytes_received += len;
        
        // Log only at progress milestones (25%, 50%, 75%, 100%) to reduce spam
        if (firmware_expected_size > 0)
        {
            uint32_t percent_now = (firmware_bytes_received * 100) / firmware_expected_size;
            static uint32_t last_logged_percent = 0;
            
            if (percent_now >= last_logged_percent + 25 || percent_now == 100)
            {
                ESP_LOGI(TAG, "OTA progress: %u%% (%u/%u bytes)", 
                         percent_now, firmware_bytes_received, firmware_expected_size);
                last_logged_percent = (percent_now / 25) * 25;
            }
        }

        // Notify OTA updater
        if (on_firmware_chunk_cb)
        {
            on_firmware_chunk_cb(data, len);
        }
    }
    else
    {
        // Regular binary message
        if (on_binary_cb)
        {
            on_binary_cb(data, len);
        }
    }
}

// Task loop sending microphone data to server
void NetworkManager::uplinkTaskLoop()
{
    const size_t SEND_SIZE = 512;
    uint8_t send_buf[SEND_SIZE];
    size_t acc = 0;

    while (started)
    {
        bool is_listening = (StateManager::instance().getInteractionState() == state::InteractionState::LISTENING);

        if (!ws_running)
            break;

        // Exit when not listening and buffer is empty
        if (!is_listening && xStreamBufferIsEmpty(mic_encoded_sb) && acc == 0)
            break;

        // Read data, waiting up to 100ms to batch enough bytes (non-busy wait)
        size_t want = SEND_SIZE - acc;
        size_t got = xStreamBufferReceive(mic_encoded_sb, send_buf + acc, want, pdMS_TO_TICKS(100));

        if (got > 0)
        {
            acc += got;
        }

        // Send immediately when 512 bytes are ready
        if (acc == SEND_SIZE)
        {
            ws->sendBinary(send_buf, SEND_SIZE);
            acc = 0;
            // Không vTaskDelay ở đây để có thể gửi liên tiếp nếu buffer đang đầy
        }

        // Flush remaining buffer once capture stops
        if (!is_listening && acc > 0 && xStreamBufferIsEmpty(mic_encoded_sb))
        {
            memset(send_buf + acc, 0, SEND_SIZE - acc);
            ws->sendBinary(send_buf, SEND_SIZE);
            break;
        }
    }
    // 4. Dọn dẹp an toàn
    if (mic_encoded_sb)
    {
        xStreamBufferReset(mic_encoded_sb);
    }
    uplink_task_handle = nullptr;
    ESP_LOGW(TAG, "Uplink task deleted");
    vTaskDelete(nullptr);
}

void NetworkManager::uplinkTaskEntry(void *arg)
{
    // Cast back to instance pointer
    auto *self = static_cast<NetworkManager *>(arg);
    if (self)
    {
        self->uplinkTaskLoop(); // Run loop (non-static)
    }
}

// ============================================================================
// PUSH STATE TO STATEMANAGER
// ============================================================================
void NetworkManager::publishState(state::ConnectivityState s)
{
    StateManager::instance().setConnectivityState(s);
}

void NetworkManager::handleInteractionState(state::InteractionState s)
{
    if (s == state::InteractionState::LISTENING)
    {
        if (uplink_task_handle == nullptr)
        {
            ESP_LOGI(TAG, "Starting Uplink Task (State: LISTENING)");
            xTaskCreatePinnedToCore(
                &NetworkManager::uplinkTaskEntry,
                "WsUplink",
                4096,
                this,
                5,
                &uplink_task_handle,
                1 // Core 0
            );
        }
    }
    else
    {
        // Khi không còn LISTENING, chúng ta không delete task từ đây
        // mà để task loop tự kiểm tra và thoát để đảm bảo an toàn dữ liệu.
    }
}

// ============================================================================
// OTA FIRMWARE UPDATE SUPPORT
// ============================================================================
bool NetworkManager::requestFirmwareUpdate(const std::string &version, uint32_t total_size, const std::string &sha256)
{
    if (!ws || !ws->isConnected())
    {
        ESP_LOGE(TAG, "WebSocket not connected, cannot request firmware");
        return false;
    }

    firmware_download_active = true;
    firmware_bytes_received = 0;
    firmware_expected_size = total_size;
    firmware_expected_sha256 = sha256;

    // Create request message (JSON format)
    std::string request = "{\"action\":\"update_firmware\"";
    if (!version.empty())
    {
        request += ",\"version\":\"" + version + "\"";
    }
    if (total_size > 0)
    {
        request += ",\"size\":" + std::to_string(total_size);
    }
    if (!sha256.empty())
    {
        request += ",\"sha256\":\"" + sha256 + "\"";
    }
    request += "}";

    ESP_LOGI(TAG, "Requesting firmware update: %s", request.c_str());
    return sendText(request);
}

void NetworkManager::onFirmwareChunk(std::function<void(const uint8_t *, size_t)> cb)
{
    on_firmware_chunk_cb = cb;
}

void NetworkManager::onFirmwareComplete(std::function<void(bool, const std::string &)> cb)
{
    on_firmware_complete_cb = cb;
}

void NetworkManager::onServerOTARequest(std::function<void()> cb)
{
    on_server_ota_request_cb = cb;
}

// Stop captive portal if running (used for low-battery mode)
void NetworkManager::stopPortal()
{
    if (wifi)
    {
        wifi->stopCaptivePortal();
    }
}

// ============================================================================
// WIFI RETRY LOGIC
// ============================================================================
void NetworkManager::retryWifiTaskEntry(void *arg)
{
    auto *self = static_cast<NetworkManager *>(arg);
    if (self)
    {
        self->retryWifiThenBLE();
    }
    vTaskDelete(nullptr);
}

void NetworkManager::retryWifiThenPortal()
{
    const int max_retries = 10; // 5 seconds / 0.5s = 10 attempts

    ESP_LOGI(TAG, "Starting WiFi retry phase (5 seconds, 10 attempts)");

    for (int attempt = 0; attempt < max_retries; attempt++)
    {
        // Check if WiFi connected during retry
        if (wifi && wifi->isConnected())
        {
            ESP_LOGI(TAG, "WiFi connected during retry phase - cancelling portal");
            wifi_retry_task = nullptr;
            return;
        }

        ESP_LOGI(TAG, "WiFi retry attempt %d/10", attempt + 1);

        // Wait 500ms
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Check one final time after all retries
    if (wifi && wifi->isConnected())
    {
        ESP_LOGI(TAG, "WiFi connected after retry phase - cancelling portal");
        wifi_retry_task = nullptr;
        return;
    }

    // 5 seconds elapsed without connection - scan then open portal
    ESP_LOGI(TAG, "WiFi retry phase complete - no connection. Scanning then opening portal...");

    if (wifi)
    {
        // Disconnect STA first to allow scanning
        wifi->disconnect();
        vTaskDelay(pdMS_TO_TICKS(100)); // Brief delay to ensure WiFi is stopped

        // Start STA mode and scan
        wifi->ensureStaStarted();
        vTaskDelay(pdMS_TO_TICKS(100)); // Brief delay before scanning

        // Scan and cache networks before opening portal
        wifi->scanAndCache();

        // Open portal with stop_wifi_first=true to ensure clean state
        wifi->startCaptivePortal(config_.ap_ssid, config_.ap_max_clients, true);
        publishState(state::ConnectivityState::WIFI_PORTAL);
    }

    wifi_retry_task = nullptr;
}

void NetworkManager::retryWifiThenBLE()
{
    const int max_retries = 10;

    ESP_LOGI(TAG, "Starting WiFi retry phase (5 seconds)");

    for (int i = 0; i < max_retries; i++)
    {
        if (wifi && wifi->isConnected())
        {
            ESP_LOGI(TAG, "WiFi connected during retry");
            wifi_retry_task = nullptr;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (wifi && wifi->isConnected())
    {
        ESP_LOGI(TAG, "WiFi connected after retry");
        wifi_retry_task = nullptr;
        return;
    }

    ESP_LOGW(TAG, "WiFi unavailable after retry - switching to BLE config mode");

    // 1. Disconnect first to stop STA from connecting
    if (wifi)
    {
        wifi->disconnect();
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for disconnect to complete
    }

    // 2. Start STA and scan networks (no longer connecting, so scan will work)
    if (wifi)
    {
        wifi->ensureStaStarted();
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for STA to be ready

        cached_networks = wifi->scanNetworks();
        ESP_LOGI(TAG, "Scanned and cached %d networks before BLE mode", cached_networks.size());
    }

    // 3. Now STOP WiFi completely to free RF for BLE
    if (wifi)
    {
        wifi->disconnect();
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for disconnect to complete
        
        // Properly stop WiFi before deinit
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for WiFi to stop
        
        esp_wifi_deinit();
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for deinit to complete
        
        ESP_LOGI(TAG, "WiFi fully stopped and deinitialized for BLE");
    }

    // 3. Publish state (BLE will be started later when RAM is freed)
    publishState(state::ConnectivityState::CONFIG_BLE);

    wifi_retry_task = nullptr;
    vTaskDelete(NULL);
}

void NetworkManager::startBLEConfigMode()
{
    if (ble_service)
    {
        ESP_LOGW(TAG, "Start BLE Config Mode now (RAM should be free)");
        ESP_LOGI(TAG, "Passing %d cached networks to BLE service", cached_networks.size());
        
        // Prepare current device configuration to restore in BLE
        BluetoothService::ConfigData current_cfg;
        current_cfg.device_name = nmgr_load_str("device_name", "PTalk");
        current_cfg.volume = nmgr_load_u8("volume", 60);
        current_cfg.brightness = nmgr_load_u8("brightness", 100);
        current_cfg.ws_url = nmgr_load_str("ws_url", "");
        
        ble_service->init(config_.ap_ssid, cached_networks, &current_cfg);
        ble_service->start();
    }
}

// Static task entry for deferred BLE config
void NetworkManager::bleConfigTaskEntry(void *arg)
{
    auto *self = static_cast<NetworkManager *>(arg);
    if (self)
    {
        self->openBLEConfigModeDeferred();
    }
    self->ble_config_task = nullptr;
    vTaskDelete(nullptr);
}

void NetworkManager::openBLEConfigMode()
{
    ESP_LOGI(TAG, "Opening BLE config mode - spawning deferred task");
    
    // CRITICAL: Cannot cleanup WebSocket from within its own callback!
    // Spawn a separate task to handle the cleanup safely
    if (ble_config_task != nullptr)
    {
        ESP_LOGW(TAG, "BLE config task already running");
        return;
    }
    
    xTaskCreate(
        &NetworkManager::bleConfigTaskEntry,
        "BLEConfig",
        6144,
        this,
        5,
        &ble_config_task
    );
}

void NetworkManager::openBLEConfigModeDeferred()
{
    ESP_LOGI(TAG, "Opening BLE config mode (deferred task)");

    // 0. STOP NETWORK MANAGER COMPLETELY
    ESP_LOGI(TAG, "Stopping all network operations...");
    
    started = false;  // Signal network loop task to exit gracefully
    ws_should_run = false;
    ws_running = false;
    
    // Wait for network loop task to exit gracefully (started=false signals it)
    // Task will set task_handle = nullptr before self-deleting
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Now safe to destroy WebSocket from outside callback context
    if (ws)
    {
        ESP_LOGI(TAG, "Destroying WebSocket...");
        ws->close();  // This can now safely destroy the client
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for WebSocket task to fully terminate
        ESP_LOGI(TAG, "WebSocket destroyed");
    }
    
    // Only try to delete task if it hasn't already self-deleted
    // (task clears its own handle before vTaskDelete(nullptr))
    if (task_handle)
    {
        ESP_LOGI(TAG, "Force deleting network task...");
        TaskHandle_t th = task_handle;
        task_handle = nullptr;
        vTaskDelete(th);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    else
    {
        ESP_LOGI(TAG, "Network task already exited");
    }
    
    ESP_LOGI(TAG, "Network operations stopped");

    // 1. Disconnect WiFi first to stop STA from connecting
    if (wifi)
    {
        wifi->disconnect();
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for disconnect to complete
    }

    // 2. Start STA and scan networks (no longer connecting, so scan will work)
    if (wifi)
    {
        wifi->ensureStaStarted();
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for STA to be ready

        cached_networks = wifi->scanNetworks();
        ESP_LOGI(TAG, "Scanned and cached %d networks before BLE mode", cached_networks.size());
    }

    // 3. Now STOP WiFi completely to free RF for BLE
    if (wifi)
    {
        wifi->disconnect();
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for disconnect to complete
        
        // Properly stop WiFi before deinit
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for WiFi to stop
        
        esp_wifi_deinit();
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for deinit to complete
        
        ESP_LOGI(TAG, "WiFi fully stopped and deinitialized for BLE");
    }

    // 4. Publish state (BLE will be started later when RAM is freed)
    publishState(state::ConnectivityState::CONFIG_BLE);

    ESP_LOGI(TAG, "BLE config mode ready - call startBLEConfigMode() when RAM is freed");
}

// ============================================================================
// EMOTION CODE PARSING
// ============================================================================
state::EmotionState NetworkManager::parseEmotionCode(const std::string &code)
{
    // Map WebSocket emotion codes to EmotionState
    // Format expected: "01", "11", etc. (2-char codes)

    if (code == "00" || code.empty())
    {
        return state::EmotionState::NEUTRAL;
    }
    if (code == "01")
    {
        return state::EmotionState::HAPPY; // Happy, cheerful
    }
    if (code == "02")
    {
        return state::EmotionState::ANGRY; // Angry, urgent
    }
    if (code == "03")
    {
        return state::EmotionState::EXCITED; // Excited, enthusiastic
    }
    if (code == "10")
    {
        return state::EmotionState::SAD; // Sad, empathetic
    }
    if (code == "12")
    {
        return state::EmotionState::CONFUSED; // Confused, uncertain
    }
    if (code == "13")
    {
        return state::EmotionState::CALM; // Calm, soothing
    }
    if (code == "99")
    {
        return state::EmotionState::THINKING; // Thinking, processing
    }

    ESP_LOGW(TAG, "Unknown emotion code: %s", code.c_str());
    return state::EmotionState::NEUTRAL;
}
// ============================================================================
// REAL-TIME WEBSOCKET CONFIGURATION
// ============================================================================

static bool nmgr_save_u8(const char* key, uint8_t value)
{
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) != ESP_OK)
        return false;
    esp_err_t e = nvs_set_u8(h, key, value);
    nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

static bool nmgr_save_str(const char* key, const std::string& value)
{
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) != ESP_OK)
        return false;
    esp_err_t e = nvs_set_str(h, key, value.c_str());
    nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

static uint8_t nmgr_load_u8(const char* key, uint8_t def_val)
{
    uint8_t v = def_val;
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v;
}

static std::string nmgr_load_str(const char* key, const char* def_val)
{
    std::string out;
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK)
    {
        size_t required = 0;
        if (nvs_get_str(h, key, nullptr, &required) == ESP_OK && required > 0)
        {
            out.resize(required);
            if (nvs_get_str(h, key, out.data(), &required) == ESP_OK)
            {
                if (!out.empty() && out.back() == '\0')
                    out.pop_back();
            }
        }
        nvs_close(h);
    }
    if (out.empty())
        out = def_val;
    return out;
}

void NetworkManager::setManagers(class AudioManager *audio, class DisplayManager *display)
{
    audio_manager = audio;
    display_manager = display;
}

void NetworkManager::onConfigUpdate(std::function<void(const std::string &, const std::string &)> cb)
{
    on_config_update_cb = cb;
}

bool NetworkManager::sendDeviceHandshake()
{
    if (!ws_running)
        return false;

    std::string device_id = getDeviceEfuseID();
    std::string app_version = app_meta::APP_VERSION;
    std::string device_name = nmgr_load_str("device_name", "PTalk");
    uint8_t battery = power_manager ? power_manager->getPercent() : 85;

    // Build handshake message with device info
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "device_handshake");
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddStringToObject(root, "firmware_version", app_version.c_str());
    cJSON_AddStringToObject(root, "device_name", device_name.c_str());
    cJSON_AddNumberToObject(root, "battery_percent", battery);
    cJSON_AddStringToObject(root, "connectivity_state", "ONLINE");

    char *json_str = cJSON_Print(root);
    bool result = sendText(json_str);
    
    cJSON_free(json_str);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Device handshake sent to server");
    return result;
}

void NetworkManager::handleConfigCommand(const std::string &json_msg)
{
    cJSON *root = cJSON_Parse(json_msg.c_str());
    if (!root)
    {
        ESP_LOGE(TAG, "Invalid JSON config command: %s", json_msg.c_str());
        return;
    }

    // Extract command type
    cJSON *cmd_obj = cJSON_GetObjectItem(root, "cmd");
    if (!cmd_obj || !cmd_obj->valuestring)
    {
        ESP_LOGE(TAG, "Config command missing or invalid 'cmd' field");
        cJSON_Delete(root);
        return;
    }

    std::string cmd_str = cmd_obj->valuestring;
    ws_config::ConfigCommand cmd = ws_config::parseCommandString(cmd_str);

    ESP_LOGI(TAG, "Processing config command: %s", cmd_str.c_str());

    // Process command
    switch (cmd)
    {
    case ws_config::ConfigCommand::SET_AUDIO_VOLUME:
    {
        cJSON *vol_obj = cJSON_GetObjectItem(root, "volume");
        if (vol_obj && vol_obj->type == cJSON_Number)
        {
            uint8_t volume = (uint8_t)vol_obj->valueint;
            applyVolumeConfig(volume);
        }
        break;
    }

    case ws_config::ConfigCommand::SET_BRIGHTNESS:
    {
        cJSON *bright_obj = cJSON_GetObjectItem(root, "brightness");
        if (bright_obj && bright_obj->type == cJSON_Number)
        {
            uint8_t brightness = (uint8_t)bright_obj->valueint;
            applyBrightnessConfig(brightness);
        }
        break;
    }

    case ws_config::ConfigCommand::SET_DEVICE_NAME:
    {
        cJSON *name_obj = cJSON_GetObjectItem(root, "device_name");
        if (name_obj && name_obj->valuestring)
        {
            applyDeviceNameConfig(name_obj->valuestring);
        }
        break;
    }

    case ws_config::ConfigCommand::SET_WIFI:
    {
        // Wi‑Fi configuration is NOT allowed via WebSocket; use BLE portal instead.
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", ws_config::statusToString(ws_config::ResponseStatus::NOT_SUPPORTED));
        cJSON_AddStringToObject(resp, "message", "WiFi config not supported over WebSocket. Use BLE.");
        cJSON_AddStringToObject(resp, "device_id", getDeviceEfuseID().c_str());
        char *resp_str = cJSON_Print(resp);
        sendText(resp_str);
        cJSON_free(resp_str);
        cJSON_Delete(resp);
        break;
    }

    case ws_config::ConfigCommand::REQUEST_STATUS:
    {
        // Send current device status
        std::string status_json = getCurrentStatusJson();
        sendText(status_json);
        break;
    }

    case ws_config::ConfigCommand::REBOOT:
    {
        cJSON *ack = cJSON_CreateObject();
        cJSON_AddStringToObject(ack, "status", "ok");
        cJSON_AddStringToObject(ack, "message", "Rebooting...");
        char *ack_str = cJSON_Print(ack);
        sendText(ack_str);
        cJSON_free(ack_str);
        cJSON_Delete(ack);
        
        // Delay before reboot to send ack
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        break;
    }

    case ws_config::ConfigCommand::REQUEST_OTA:
    {
        // Server is initiating OTA - prepare to receive binary data
        uint32_t fw_size = 0;
        cJSON *size_obj = cJSON_GetObjectItem(root, "size");
        if (size_obj && cJSON_IsNumber(size_obj))
        {
            fw_size = static_cast<uint32_t>(size_obj->valuedouble);
        }

        std::string fw_sha256;
        cJSON *sha_obj = cJSON_GetObjectItem(root, "sha256");
        if (sha_obj && sha_obj->valuestring)
        {
            fw_sha256 = sha_obj->valuestring;
        }

        // Setup OTA state to receive binary data
        firmware_download_active = true;
        firmware_bytes_received = 0;
        firmware_expected_size = fw_size;
        firmware_expected_sha256 = fw_sha256;

        ESP_LOGI(TAG, "OTA initiated by server: size=%u, sha256=%s", fw_size, fw_sha256.c_str());

        // CRITICAL: Notify AppController to setup OTA callbacks BEFORE sending ACK
        // This ensures callbacks are registered before binary data arrives
        if (on_server_ota_request_cb)
        {
            ESP_LOGI(TAG, "Calling server OTA request callback to setup handlers...");
            on_server_ota_request_cb();
        }
        else
        {
            ESP_LOGW(TAG, "No server OTA request callback registered - OTA may fail!");
        }

        // Send ACK - ready to receive firmware
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "message", "Ready to receive firmware");
        if (fw_size > 0)
            cJSON_AddNumberToObject(resp, "size", fw_size);
        if (!fw_sha256.empty())
            cJSON_AddStringToObject(resp, "sha256", fw_sha256.c_str());
        cJSON_AddStringToObject(resp, "device_id", getDeviceEfuseID().c_str());

        char *resp_str = cJSON_Print(resp);
        sendText(resp_str);
        cJSON_free(resp_str);
        cJSON_Delete(resp);
        break;
    }

    case ws_config::ConfigCommand::REQUEST_BLE_CONFIG:
    {
        cJSON *ack = cJSON_CreateObject();
        cJSON_AddStringToObject(ack, "status", "ok");
        cJSON_AddStringToObject(ack, "message", "Opening BLE config mode...");
        cJSON_AddStringToObject(ack, "device_id", getDeviceEfuseID().c_str());
        char *ack_str = cJSON_Print(ack);
        sendText(ack_str);
        cJSON_free(ack_str);
        cJSON_Delete(ack);
        
        // Delay before opening BLE to send ack
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Open BLE config mode (this will scan WiFi and prepare for BLE)
        openBLEConfigMode();
        break;
    }

    default:
        ESP_LOGW(TAG, "Unknown config command: %s", cmd_str.c_str());
        break;
    }

    cJSON_Delete(root);
}

bool NetworkManager::applyVolumeConfig(uint8_t volume)
{
    if (volume > 100)
        volume = 100;

    ESP_LOGI(TAG, "Applying volume config: %d%%", volume);

    if (audio_manager)
    {
        audio_manager->setVolume(volume);
    }

    // Persist to NVS for next boot
    nmgr_save_u8("volume", volume);

    // Send response
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "ok");
    cJSON_AddNumberToObject(response, "volume", volume);
    cJSON_AddStringToObject(response, "device_id", getDeviceEfuseID().c_str());
    char *resp_str = cJSON_Print(response);
    
    bool result = sendText(resp_str);
    
    cJSON_free(resp_str);
    cJSON_Delete(response);

    // Callback
    if (on_config_update_cb)
        on_config_update_cb("volume", std::to_string(volume));

    return result;
}

bool NetworkManager::applyBrightnessConfig(uint8_t brightness)
{
    if (brightness > 100)
        brightness = 100;

    ESP_LOGI(TAG, "Applying brightness config: %d%%", brightness);

    if (display_manager)
    {
        display_manager->setBrightness(brightness);
    }

    // Persist to NVS for next boot
    nmgr_save_u8("brightness", brightness);

    // Send response
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "ok");
    cJSON_AddNumberToObject(response, "brightness", brightness);
    cJSON_AddStringToObject(response, "device_id", getDeviceEfuseID().c_str());
    char *resp_str = cJSON_Print(response);
    
    bool result = sendText(resp_str);
    
    cJSON_free(resp_str);
    cJSON_Delete(response);

    // Callback
    if (on_config_update_cb)
        on_config_update_cb("brightness", std::to_string(brightness));

    return result;
}

bool NetworkManager::applyDeviceNameConfig(const std::string &name)
{
    ESP_LOGI(TAG, "Applying device name config: %s", name.c_str());

    // Persist to NVS for next boot
    nmgr_save_str("device_name", name);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "ok");
    cJSON_AddStringToObject(response, "device_name", name.c_str());
    cJSON_AddStringToObject(response, "device_id", getDeviceEfuseID().c_str());
    char *resp_str = cJSON_Print(response);
    
    bool result = sendText(resp_str);
    
    cJSON_free(resp_str);
    cJSON_Delete(response);

    // Callback
    if (on_config_update_cb)
        on_config_update_cb("device_name", name);

    return result;
}

bool NetworkManager::applyWiFiConfig(const std::string &ssid, const std::string &password)
{
    ESP_LOGI(TAG, "Applying WiFi config: SSID=%s", ssid.c_str());

    // Save to NVS (via setCredentials)
    setCredentials(ssid, password);

    // Send response before restart
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "ok");
    cJSON_AddStringToObject(response, "message", "WiFi configured, restarting...");
    cJSON_AddStringToObject(response, "device_id", getDeviceEfuseID().c_str());
    char *resp_str = cJSON_Print(response);
    
    bool result = sendText(resp_str);
    
    cJSON_free(resp_str);
    cJSON_Delete(response);

    // Delay before restart
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return result;
}

std::string NetworkManager::getCurrentStatusJson() const
{
    std::string device_id = getDeviceEfuseID();
    std::string app_version = app_meta::APP_VERSION;
    std::string device_name = nmgr_load_str("device_name", "PTalk");
    uint8_t volume = nmgr_load_u8("volume", 60);
    uint8_t brightness = nmgr_load_u8("brightness", 100);
    uint8_t battery = power_manager ? power_manager->getPercent() : 85;
    uint32_t uptime_sec = static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddStringToObject(root, "device_name", device_name.c_str());
    cJSON_AddNumberToObject(root, "battery_percent", battery);
    cJSON_AddStringToObject(root, "connectivity_state", "ONLINE");
    cJSON_AddStringToObject(root, "firmware_version", app_version.c_str());
    cJSON_AddNumberToObject(root, "volume", volume); // From NVS (WS/BLE persisted)
    cJSON_AddNumberToObject(root, "brightness", brightness); // From NVS (WS/BLE persisted)
    cJSON_AddNumberToObject(root, "uptime_sec", uptime_sec);

    char *json_str = cJSON_Print(root);
    std::string result(json_str);
    
    cJSON_free(json_str);
    cJSON_Delete(root);

    return result;
}