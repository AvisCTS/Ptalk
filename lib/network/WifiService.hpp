#pragma once

#include <string>
#include <vector>
#include <functional>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_err.h"

/**
 * WifiInfo
 * ---------------------------------------------------------
 * - Kết quả scan wifi
 */
struct WifiInfo {
    std::string ssid;
    int rssi = -99;
};

/**
 * HandlerContext for HTTP server
 * ---------------------------------------------------------
 * - Context truyền vào HTTP handlers
 */

class WifiService;

struct HandlerContext
{
    WifiService *svc;
};

/**
 * WifiService
 * ---------------------------------------------------------
 * - Init WiFi stack (NVS, netif, wifi driver)
 * - Auto-connect STA nếu có credentials
 * - Nếu không → mở Captive Portal
 * - Cung cấp scan WiFi
 * - Callback khi WiFi CONNECTING / CONNECTED / DISCONNECTED
 */
class WifiService {
public:
    WifiService() = default;
    ~WifiService() = default;

    // Init WiFi môi trường (NVS, netif, event loop)
    void init();

    // Bắt đầu auto-connect STA (return false nếu không có SSID/PASS)
    bool autoConnect();

    // Bật portal
    void startCaptivePortal(const std::string& ap_ssid = "PTalk", const uint8_t ap_num_connections = 4, bool stop_wifi_first = false);
    void stopCaptivePortal();

    // Ngắt STA
    void disconnect();

    // Tắt auto reconnect (theo yêu cầu người dùng)
    void disableAutoConnect();

    // Trạng thái
    bool isConnected() const { return connected; }
    std::string getIp() const;
    std::string getSsid() const { return sta_ssid; }

    // Credential
    void connectWithCredentials(const char* ssid, const char* pass);

    // Scan WiFi
    std::vector<WifiInfo> scanNetworks();
    void scanAndCache();  // Scan and cache networks (to be called before portal)
    std::vector<WifiInfo> getCachedNetworks() const { return cached_networks; }
    void ensureStaStarted(); // Ensure STA mode is started

    // Callback status: 0=DISCONNECTED, 1=CONNECTING, 2=GOT_IP
    void onStatus(std::function<void(int)> cb) { status_cb = cb; }

    // Control AP-only mode from external (e.g., HTTP handlers)
    void setApOnlyMode(bool enabled) { ap_only_mode = enabled; }

private:
    void loadCredentials();
    void saveCredentials(const char* ssid, const char* pass);
    void startSTA();
    void registerEvents();

    // Event handlers
    static void wifiEventHandlerStatic(void* arg, esp_event_base_t base,
                                      int32_t id, void* data);

    static void ipEventHandlerStatic(void* arg, esp_event_base_t base,
                                     int32_t id, void* data);

    void wifiEventHandler(esp_event_base_t base, int32_t id, void* data);
    void ipEventHandler(esp_event_base_t base, int32_t id, void* data);

private:
    std::string sta_ssid;
    std::string sta_pass;

    bool connected = false;
    bool auto_connect_enabled = true;
    bool portal_running = false;
    // When true, WifiService should operate in AP-only mode and ignore STA events
    bool ap_only_mode = false;
    bool has_connected_once = false;  // Track if WiFi ever connected successfully
    bool wifi_started = false;  // Track if WiFi has been started

    esp_netif_t* sta_netif = nullptr;
    esp_netif_t* ap_netif = nullptr;

    httpd_handle_t http_server = nullptr;
    HandlerContext http_ctx{ this };
    std::vector<WifiInfo> cached_networks;  // Cache WiFi scan results

    std::function<void(int)> status_cb = nullptr;
};
