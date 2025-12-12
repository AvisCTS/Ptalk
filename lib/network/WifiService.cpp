#include "WifiService.hpp"

#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "web_page.hpp"
#include "logo1.hpp"
#include "logo2.hpp"

#include <algorithm>

static const char* TAG = "WifiService";

#define NVS_NS   "wifi"
#define NVS_SSID "ssid"
#define NVS_PASS "pass"

static void replaceStr(std::string& src, const std::string& from, const std::string& to)
{
    size_t pos = 0;
    while ((pos = src.find(from, pos)) != std::string::npos) {
        src.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::string urldecode(const std::string& s)
{
    std::string out;
    char ch;
    int val;

    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) {
            sscanf(s.substr(i+1, 2).c_str(), "%x", &val);
            ch = (char)val;
            out += ch;
            i += 2;
        } else out += s[i];
    }
    return out;
}

// ============================================================================
// HTTP Portal handlers
// ============================================================================
esp_err_t portal_GET_handler(httpd_req_t* req)
{
    auto* self = (WifiService*)req->user_ctx;
    auto nets  = self->scanNetworks();

    std::string list;

    for (auto& n : nets) {
        int quality = (n.rssi <= -100) ? 0 : (n.rssi >= -50 ? 100 : 2 * (n.rssi + 100));
        std::string bar_color = (quality > 60) ? "#48bb78" :
                                (quality > 30) ? "#ecc94b" : "#f56565";

        list += "<div class='wifi-item' onclick=\"sel('" + n.ssid + "')\">";
        list += "<span class='ssid-text'>" + n.ssid + "</span>";
        list += "<div class='rssi-box'>" + std::to_string(n.rssi) + " dBm";
        list += "<div class='bar-bg'><div class='bar-fg' style='width:" + std::to_string(quality);
        list += "%; background:" + bar_color + ";'></div></div>";
        list += "</div></div>";
    }

    std::string page = PAGE_HTML;
    replaceStr(page, "%WIFI_LIST%", list);
    replaceStr(page, "%LOGO1%", LOGO1_DATA);
    replaceStr(page, "%LOGO2%", LOGO2_DATA);

    httpd_resp_send(req, page.c_str(), page.size());
    return ESP_OK;
}

esp_err_t portal_POST_handler(httpd_req_t* req)
{
    auto* self = (WifiService*)req->user_ctx;

    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = 0;

    std::string body(buf);
    auto pos_ssid = body.find("ssid=");
    auto pos_pass = body.find("&pass=");

    std::string ssid = body.substr(pos_ssid + 5, pos_pass - (pos_ssid + 5));
    std::string pass = body.substr(pos_pass + 6);

    ssid = urldecode(ssid);
    pass = urldecode(pass);

    ESP_LOGI(TAG, "Portal received SSID=%s PASS=%s", ssid.c_str(), pass.c_str());

    self->connectWithCredentials(ssid.c_str(), pass.c_str());

    httpd_resp_sendstr(req, "OK, rebooting WiFi...");
    return ESP_OK;
}

// ============================================================================
// INIT
// ============================================================================
void WifiService::init()
{
    // NVS init
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());

    registerEvents();
    loadCredentials();
}

// ============================================================================
// AUTO CONNECT
// ============================================================================
bool WifiService::autoConnect()
{
    if (!auto_connect_enabled) return false;

    if (sta_ssid.empty() || sta_pass.empty()) {
        ESP_LOGW(TAG, "No credentials → opening portal");
        startCaptivePortal();
        return false;
    }

    startSTA();
    return true;
}

// ============================================================================
// START STA
// ============================================================================
void WifiService::startSTA()
{
    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid, sta_ssid.c_str(), sizeof(cfg.sta.ssid));
    strncpy((char*)cfg.sta.password, sta_pass.c_str(), sizeof(cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    if (status_cb) status_cb(1); // CONNECTING
}

// ============================================================================
// START CAPTIVE PORTAL
// ============================================================================
void WifiService::startCaptivePortal(const std::string& ap_ssid, uint8_t ap_num_connections)
{
    if (portal_running) return;

    ESP_LOGI(TAG, "Starting Captive Portal: SSID=%s max_conn=%d",
             ap_ssid.c_str(), ap_num_connections);

    wifi_config_t cfg = {};
    strncpy((char*)cfg.ap.ssid, ap_ssid.c_str(), sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len       = ap_ssid.size();
    cfg.ap.authmode       = WIFI_AUTH_OPEN;
    cfg.ap.max_connection = ap_num_connections;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));

    // HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;

    if (httpd_start(&http_server, &config) == ESP_OK)
    {
        httpd_uri_t get = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = portal_GET_handler,
            .user_ctx = this,
        };
        httpd_register_uri_handler(http_server, &get);

        httpd_uri_t post = {
            .uri      = "/connect",
            .method   = HTTP_POST,
            .handler  = portal_POST_handler,
            .user_ctx = this,
        };
        httpd_register_uri_handler(http_server, &post);
    }

    portal_running = true;
}

// ============================================================================
// STOP PORTAL
// ============================================================================
void WifiService::stopCaptivePortal()
{
    if (!portal_running) return;

    ESP_LOGI(TAG, "Stopping Captive Portal");

    if (http_server) {
        httpd_stop(http_server);
        http_server = nullptr;
    }

    portal_running = false;
}

// ============================================================================
// DISCONNECT
// ============================================================================
void WifiService::disconnect()
{
    ESP_LOGW(TAG, "WiFi Disconnect");
    esp_wifi_disconnect();
    connected = false;
    if (status_cb) status_cb(0);
}

// ============================================================================
// SCAN NETWORKS
// ============================================================================
std::vector<WifiInfo> WifiService::scanNetworks()
{
    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;

    ESP_ERROR_CHECK(esp_wifi_scan_start(&cfg, true));

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

    std::vector<wifi_ap_record_t> records(ap_count);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, records.data()));

    std::vector<WifiInfo> out;
    for (auto& r : records) {
        if (r.ssid[0] == '\0') continue;
        WifiInfo info;
        info.ssid = (char*)r.ssid;
        info.rssi = r.rssi;
        out.push_back(info);
    }

    std::sort(out.begin(), out.end(), [](auto& a, auto& b){ return a.rssi > b.rssi; });
    return out;
}

// ============================================================================
// CREDENTIALS
// ============================================================================
void WifiService::connectWithCredentials(const char* ssid, const char* pass)
{
    saveCredentials(ssid, pass);
    startSTA();
}

void WifiService::loadCredentials()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        sta_ssid.clear();
        sta_pass.clear();
        return;
    }

    // SSID
    size_t len = 0;
    if (nvs_get_str(h, NVS_SSID, nullptr, &len) == ESP_OK && len > 1) {
        std::string buf(len, 0);
        nvs_get_str(h, NVS_SSID, buf.data(), &len);
        sta_ssid = buf.c_str();
    }

    // PASS
    len = 0;
    if (nvs_get_str(h, NVS_PASS, nullptr, &len) == ESP_OK && len > 1) {
        std::string buf(len, 0);
        nvs_get_str(h, NVS_PASS, buf.data(), &len);
        sta_pass = buf.c_str();
    }

    nvs_close(h);

    ESP_LOGI(TAG, "Credentials loaded: SSID=%s PASS=%s",
             sta_ssid.c_str(), sta_pass.empty() ? "(empty)" : "****");
}

void WifiService::saveCredentials(const char* ssid, const char* pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));

    ESP_ERROR_CHECK(nvs_set_str(h, NVS_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, NVS_PASS, pass));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);

    sta_ssid = ssid;
    sta_pass = pass;

    ESP_LOGI(TAG, "Credentials saved: %s / %s", ssid, pass);
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================
void WifiService::registerEvents()
{
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &WifiService::wifiEventHandlerStatic, this, nullptr));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &WifiService::ipEventHandlerStatic, this, nullptr));
}

void WifiService::wifiEventHandlerStatic(void* arg, esp_event_base_t base,
                                         int32_t id, void* data)
{
    ((WifiService*)arg)->wifiEventHandler(base, id, data);
}

void WifiService::ipEventHandlerStatic(void* arg, esp_event_base_t base,
                                       int32_t id, void* data)
{
    ((WifiService*)arg)->ipEventHandler(base, id, data);
}

void WifiService::wifiEventHandler(esp_event_base_t base, int32_t id, void* data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA start");
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "STA connected");
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        connected = false;
        if (status_cb) status_cb(0);

        if (auto_connect_enabled) {
            ESP_LOGW(TAG, "Retry STA connect");
            esp_wifi_connect();
        } else {
            startCaptivePortal();
        }
        break;
    }
}

void WifiService::ipEventHandler(esp_event_base_t base, int32_t id, void* data)
{
    if (id != IP_EVENT_STA_GOT_IP) return;

    connected = true;
    if (status_cb) status_cb(2);

    ESP_LOGI(TAG, "Got IP");
}

std::string WifiService::getIp() const
{
    if (!connected) return "";

    esp_netif_ip_info_t ip;
    esp_netif_t* n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n && esp_netif_get_ip_info(n, &ip) == ESP_OK) {
        char buf[32];
        sprintf(buf, IPSTR, IP2STR(&ip.ip));
        return buf;
    }
    return "";
}
