#include "WebSocketClient.hpp"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include <cstring>

static const char* TAG = "WebSocketClient";

#define WEBSOCKET_OPCODE_TEXT    0x01
#define WEBSOCKET_OPCODE_BINARY  0x02
#define WEBSOCKET_OPCODE_CLOSE   0x08
#define WEBSOCKET_OPCODE_PING    0x09
#define WEBSOCKET_OPCODE_PONG    0x0A

WebSocketClient::WebSocketClient() = default;

WebSocketClient::~WebSocketClient() {
    disconnect();
}

// ======================================================================
// CONNECT
// ======================================================================
bool WebSocketClient::connect(const std::string& url)
{
    ESP_LOGI(TAG, "Connecting WS → %s", url.c_str());

    if (ws_) {
        ESP_LOGW(TAG, "WS already exists, closing old one");
        disconnect();
    }

    current_url_ = url;

    esp_websocket_client_config_t cfg = {};
    cfg.uri = current_url_.c_str();
    cfg.disable_auto_reconnect = true;   // We control reconnect in NetworkManager
    cfg.buffer_size = 4096;
    cfg.task_stack = 4 * 1024;
    cfg.task_prio  = 5;

    ws_ = esp_websocket_client_init(&cfg);
    if (!ws_) {
        ESP_LOGE(TAG, "Failed to create websocket client");
        return false;
    }

    // Register event handler
    ESP_ERROR_CHECK(esp_websocket_register_events(
        ws_,
        WEBSOCKET_EVENT_ANY,
        &WebSocketClient::wsEventHandler,
        this
    ));

    connected_ = false;
    if (status_cb_) status_cb_(1); // CONNECTING

    esp_err_t err = esp_websocket_client_start(ws_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ws start failed: %s", esp_err_to_name(err));
        if (status_cb_) status_cb_(0); // DISCONNECTED
        return false;
    }

    return true;
}

// ======================================================================
// DISCONNECT
// ======================================================================
void WebSocketClient::disconnect()
{
    if (!ws_) return;

    ESP_LOGW(TAG, "Closing WebSocket");

    esp_websocket_client_stop(ws_);
    esp_websocket_client_destroy(ws_);
    ws_ = nullptr;

    connected_ = false;
    if (status_cb_) status_cb_(0); // DISCONNECTED
}

// ======================================================================
// SEND TEXT
// ======================================================================
bool WebSocketClient::sendText(const std::string& text)
{
    if (!ws_ || !connected_) return false;

    esp_err_t err = esp_websocket_client_send_text(
        ws_,
        text.c_str(),
        text.length(),
        portMAX_DELAY
    );

    return (err == ESP_OK);
}

// ======================================================================
// SEND BINARY
// ======================================================================
bool WebSocketClient::sendBinary(const uint8_t* data, size_t len)
{
    if (!ws_ || !connected_) return false;

    esp_err_t err = esp_websocket_client_send_bin(
        ws_,
        (const char*)data,
        len,
        portMAX_DELAY
    );

    return (err == ESP_OK);
}

// ======================================================================
// EVENT HANDLER (STATIC)
// ======================================================================
void WebSocketClient::wsEventHandler(void* handler_args,
                                     esp_event_base_t base,
                                     int32_t event_id,
                                     void* event_data)
{
    WebSocketClient* self = static_cast<WebSocketClient*>(handler_args);
    auto* data = (esp_websocket_event_data_t*)event_data;

    switch (event_id) {

    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS EVENT → CONNECTED");
        self->handleConnected();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WS EVENT → DISCONNECTED");
        self->handleDisconnected();
        break;

    case WEBSOCKET_EVENT_DATA:
        {
            if (data->op_code == WEBSOCKET_OPCODE_TEXT) {
                self->handleTextMessage((const char*)data->data_ptr, data->data_len);
            } else if (data->op_code == WEBSOCKET_OPCODE_BINARY) {
                self->handleBinaryMessage((const uint8_t*)data->data_ptr, data->data_len);
            }
            break;
        }


    default:
        break;
    }
}

// ======================================================================
// INTERNAL HANDLERS
// ======================================================================
void WebSocketClient::handleConnected()
{
    connected_ = true;
    if (status_cb_) status_cb_(2);  // CONNECTED
}

void WebSocketClient::handleDisconnected()
{
    connected_ = false;
    if (status_cb_) status_cb_(0);  // DISCONNECTED
}

void WebSocketClient::handleTextMessage(const char* data, size_t len)
{
    if (text_cb_) {
        std::string msg(data, len);
        text_cb_(msg);
    }
}

void WebSocketClient::handleBinaryMessage(const uint8_t* data, size_t len)
{
    if (bin_cb_) {
        bin_cb_(data, len);
    }
}
