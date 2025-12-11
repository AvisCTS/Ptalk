#pragma once

#include <string>
#include <functional>
#include <vector>
#include <cstdint>

#include "esp_websocket_client.h"
#include "esp_event.h"
#include "esp_log.h"


/**
 * WebSocketClient
 * --------------------------
 *  - Thin wrapper around esp_websocket_client
 *  - Provides clean callbacks:
 *       status: 0=DISCONNECTED, 1=CONNECTING, 2=CONNECTED
 *       text message
 *       binary message
 */
class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();

    // ----------------------------------------------------------------------
    // Connection control
    // ----------------------------------------------------------------------
    bool connect(const std::string& url);
    void disconnect();
    bool isConnected() const { return connected_; }

    // ----------------------------------------------------------------------
    // Sending data
    // ----------------------------------------------------------------------
    bool sendText(const std::string& text);
    bool sendBinary(const uint8_t* data, size_t len);

    // ----------------------------------------------------------------------
    // Callback setters
    // ----------------------------------------------------------------------
    // status: 0=DISCONNECTED, 1=CONNECTING, 2=CONNECTED
    void onStatus(std::function<void(int)> cb) { status_cb_ = cb; }

    // text message callback
    void onTextMessage(std::function<void(const std::string&)> cb) { text_cb_ = cb; }

    // binary message callback
    void onBinaryMessage(std::function<void(const uint8_t*, size_t)> cb) { bin_cb_ = cb; }

private:
    // Internal event handler
    static void wsEventHandler(void* handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void* event_data);

    // Dispatchers
    void handleConnected();
    void handleDisconnected();
    void handleTextMessage(const char* data, size_t len);
    void handleBinaryMessage(const uint8_t* data, size_t len);

private:
    esp_websocket_client_handle_t ws_ = nullptr;
    bool connected_ = false;

    std::string current_url_;

    // callbacks
    std::function<void(int)> status_cb_;
    std::function<void(const std::string&)> text_cb_;
    std::function<void(const uint8_t*, size_t)> bin_cb_;
};
