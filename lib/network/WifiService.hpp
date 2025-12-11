#pragma once

#include <string>
#include <vector>
#include <functional>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"

struct WifiInfo {
    std::string ssid;
    int rssi;
};

class WifiService {
public:
    static WifiService& instance();

    // ======================================================
    // Core API
    // ======================================================
    bool init();                 // NVS + WiFi driver + event loop + netif
    bool autoConnect();          // Connect using saved credential
    void connectWithCredentials(const char* ssid, const char* pass);

    void startCaptivePortal();   // Show your existing HTML portal
    void stopCaptivePortal();
    void disconnect();
    void disableAutoConnect();

    // ======================================================
    // Query
    // ======================================================
    bool isConnected() const { return wifi_connected; }
    std::string getIp() const;
    std::string getSsid() const { return sta_ssid; }
    std::string getPassword() const { return sta_pass; }

    void scanNetworks(std::vector<WifiInfo>& out);

    // ======================================================
    // Callbacks → NetworkManager will catch these
    // Status: 0 = DISCONNECTED, 1 = CONNECTING, 2 = GOT_IP
    // ======================================================
    void onStatus(std::function<void(int)> cb) { status_callback = cb; }

private:
    WifiService() = default;

    // Credential
    void loadCredentials();
    void saveCredentials(const char* ssid, const char* pass);

    // WiFi actions
    void startSTA();
    void registerEvents();

    // Event handlers
    static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data);
    static void ip_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data);

private:
    // WiFi state
    std::string sta_ssid;
    std::string sta_pass;

    bool auto_connect_enabled = true;
    bool wifi_connected = false;
    bool portal_running = false;

    esp_netif_t* sta_netif = nullptr;
    esp_netif_t* ap_netif = nullptr;

    httpd_handle_t portal_server = nullptr;

    // Callback to NetworkManager
    std::function<void(int)> status_callback = nullptr;
};
