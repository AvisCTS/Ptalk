// WifiService.cpp
#include "WifiService.hpp"

#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"

#include "web_page.hpp"  // PAGE_HTML
#include "logo1.hpp"     // LOGO1_DATA
#include "logo2.hpp"     // LOGO2_DATA

#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>

static const char* TAG = "WifiService";

#define NVS_NAMESPACE "wifi"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

#define PORTAL_SSID "ESP32_Config"
#define PORTAL_MAX_CONNECTIONS 4

// ---------- helpers ----------
static void replaceString(std::string& subject, const std::string& search, const std::string& replace) {
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != std::string::npos) {
        subject.replace(pos, search.length(), replace);
        pos += replace.length();
    }
}

static std::string urlDecode(const std::string& src) {
    std::string ret;
    char ch;
    int ii;
    for (size_t i = 0; i < src.length(); i++) {
        if (src[i] == '+') {
            ret += ' ';
        } else if (src[i] == '%' && i + 2 < src.length()) {
            sscanf(src.substr(i+1, 2).c_str(), "%x", &ii);
            ch = static_cast<char>(ii);
            ret += ch;
            i += 2;
        } else {
            ret += src[i];
        }
    }
    return ret;
}

// Forward declare instance() (defined below)
WifiService& WifiService::instance() {
    static WifiService inst;
    return inst;
}

// ---------- Portal handlers (use instance()) ----------
static esp_err_t portal_get_handler(httpd_req_t *req) {
    // 1. Scan networks
    std::vector<WifiInfo> networks;
    WifiService::instance().scanNetworks(networks);

    // Build list HTML
    std::string listHtml;
    listHtml.reserve(networks.size() * 64);

    for (auto& net : networks) {
        int quality = (net.rssi <= -100) ? 0 : (net.rssi >= -50 ? 100 : 2 * (net.rssi + 100));
        std::string color = (quality > 60) ? "#48bb78" : ((quality > 30) ? "#ecc94b" : "#f56565");

        // escape ssid minimal
        std::string ssid = net.ssid;
        // (If you need html-escape, add here)

        listHtml += "<div class='wifi-item' onclick=\"sel('" + ssid + "')\">";
        listHtml +=   "<span class='ssid-text'>" + ssid + "</span>";
        listHtml +=   "<div class='rssi-box'>" + std::to_string(net.rssi) + " dBm";
        listHtml +=      "<div class='bar-bg'><div class='bar-fg' style='width:" + std::to_string(quality) + "%; background:" + color + ";'></div></div>";
        listHtml +=   "</div></div>";
    }

    // Compose final page from PAGE_HTML
    std::string page = PAGE_HTML;
    replaceString(page, "%WIFI_LIST%", listHtml);
    replaceString(page, "%LOGO1%", LOGO1_DATA);
    replaceString(page, "%LOGO2%", LOGO2_DATA);

    httpd_resp_send(req, page.c_str(), page.size());
    return ESP_OK;
}

static esp_err_t portal_post_handler(httpd_req_t *req) {
    // Read body (small form)
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (ret <= 0) {
        httpd_resp_sendstr(req, "Invalid request");
        return ESP_FAIL;
    }
    buf[ret] = 0;
    std::string body(buf);

    // Expecting form like: ssid=<...>&pass=<...> or s=<...>&p=<...>
    // Try to locate common keys
    std::string ssid, pass;
    auto pos_ssid = body.find("ssid=");
    auto pos_pass = body.find("&pass=");
    if (pos_ssid != std::string::npos && pos_pass != std::string::npos) {
        ssid = body.substr(pos_ssid + 5, pos_pass - (pos_ssid + 5));
        pass = body.substr(pos_pass + 6);
    } else {
        // try short keys
        auto ps = body.find("s=");
        auto pp = body.find("&p=");
        if (ps != std::string::npos && pp != std::string::npos) {
            ssid = body.substr(ps + 2, pp - (ps + 2));
            pass = body.substr(pp + 3);
        }
    }

    // URL decode
    ssid = urlDecode(ssid);
    pass = urlDecode(pass);

    // Save and connect
    WifiService::instance().connectWithCredentials(ssid.c_str(), pass.c_str());

    httpd_resp_sendstr(req, "Saved! Device will attempt to connect.");
    return ESP_OK;
}

// ---------- WifiService methods ----------
bool WifiService::init() {
    ESP_LOGI(TAG, "WifiService init");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // create default netifs for STA+AP (safe even if not used)
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // register event handlers
    registerEvents();

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    // load credentials (NVS)
    loadCredentials();

    return true;
}

bool WifiService::autoConnect() {
    if (!auto_connect_enabled) {
        ESP_LOGI(TAG, "Auto connect disabled");
        return false;
    }

    if (sta_ssid.empty()) {
        ESP_LOGW(TAG, "No credentials, start captive portal");
        startCaptivePortal();
        // notify callback that we are in portal mode (disconnected)
        if (status_callback) status_callback(0);
        return false;
    }

    startSTA();
    return true;
}

void WifiService::startSTA() {
    ESP_LOGI(TAG, "Start STA with SSID='%s'", sta_ssid.c_str());

    wifi_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    strncpy((char*)wcfg.sta.ssid, sta_ssid.c_str(), sizeof(wcfg.sta.ssid) - 1);
    strncpy((char*)wcfg.sta.password, sta_pass.c_str(), sizeof(wcfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    if (status_callback) status_callback(1); // CONNECTING
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect() returned %d", err);
    }
}

void WifiService::startCaptivePortal() {
    if (portal_running) return;
    ESP_LOGI(TAG, "Starting captive portal (SSID=%s, max=%d)", PORTAL_SSID, PORTAL_MAX_CONNECTIONS);

    wifi_config_t apcfg;
    memset(&apcfg, 0, sizeof(apcfg));
    strncpy((char*)apcfg.ap.ssid, PORTAL_SSID, sizeof(apcfg.ap.ssid) - 1);
    apcfg.ap.ssid_len = strlen(PORTAL_SSID);
    apcfg.ap.max_connection = PORTAL_MAX_CONNECTIONS;
    apcfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apcfg));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    // keep some margin
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 4096;

    if (httpd_start(&portal_server, &cfg) == ESP_OK) {
        httpd_uri_t uri_get = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = portal_get_handler,
            .user_ctx = nullptr
        };
        httpd_register_uri_handler(portal_server, &uri_get);

        httpd_uri_t uri_post = {
            .uri = "/connect",
            .method = HTTP_POST,
            .handler = portal_post_handler,
            .user_ctx = nullptr
        };
        httpd_register_uri_handler(portal_server, &uri_post);
    } else {
        ESP_LOGE(TAG, "Failed to start portal HTTP server");
    }

    portal_running = true;
}

void WifiService::stopCaptivePortal() {
    if (!portal_running) return;
    ESP_LOGI(TAG, "Stopping captive portal");
    if (portal_server) {
        httpd_stop(portal_server);
        portal_server = nullptr;
    }
    portal_running = false;
}

void WifiService::disconnect() {
    ESP_LOGI(TAG, "Disconnecting WiFi");
    esp_wifi_disconnect();
    wifi_connected = false;
    if (status_callback) status_callback(0);
}

void WifiService::disableAutoConnect() {
    auto_connect_enabled = false;
}

std::string WifiService::getIp() const {
    if (!wifi_connected) return std::string();

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
        char buf[32];
        sprintf(buf, IPSTR, IP2STR(&ip_info.ip));
        return std::string(buf);
    }
    return std::string();
}

void WifiService::scanNetworks(std::vector<WifiInfo>& out) {
    out.clear();

    wifi_scan_config_t scan_cfg;
    memset(&scan_cfg, 0, sizeof(scan_cfg));
    scan_cfg.show_hidden = false;

    // blocking scan
    esp_err_t r = esp_wifi_scan_start(&scan_cfg, true);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %d", r);
        return;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) return;

    wifi_ap_record_t* rec = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (!rec) return;

    esp_wifi_scan_get_ap_records(&ap_num, rec);

    for (int i = 0; i < (int)ap_num; ++i) {
        if (rec[i].ssid[0] == 0) continue;
        WifiInfo wi;
        wi.ssid = std::string(reinterpret_cast<char*>(rec[i].ssid));
        wi.rssi = rec[i].rssi;
        out.push_back(wi);
    }

    free(rec);

    // sort by rssi desc
    std::sort(out.begin(), out.end(), [](const WifiInfo& a, const WifiInfo& b){
        return a.rssi > b.rssi;
    });
}

void WifiService::connectWithCredentials(const char* ssid, const char* pass) {
    if (!ssid) return;
    saveCredentials(ssid, pass ? pass : "");
    stopCaptivePortal();
    startSTA();
}

void WifiService::loadCredentials() {
    nvs_handle_t h;
    esp_err_t r = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (r != ESP_OK) {
        sta_ssid.clear();
        sta_pass.clear();
        return;
    }

    char buf[128];
    size_t sz = sizeof(buf);
    if (nvs_get_str(h, NVS_KEY_SSID, buf, &sz) == ESP_OK) {
        sta_ssid = std::string(buf);
    } else {
        sta_ssid.clear();
    }

    sz = sizeof(buf);
    if (nvs_get_str(h, NVS_KEY_PASS, buf, &sz) == ESP_OK) {
        sta_pass = std::string(buf);
    } else {
        sta_pass.clear();
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded WiFi creds: SSID='%s' PASS='%s'", sta_ssid.c_str(), sta_pass.empty() ? "(empty)" : "****");
}

void WifiService::saveCredentials(const char* ssid, const char* pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed");
        return;
    }

    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);

    sta_ssid = ssid;
    sta_pass = pass ? pass : "";

    ESP_LOGI(TAG, "Saved WiFi creds");
}

void WifiService::registerEvents() {
    // prefer instance-based registration
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiService::wifi_event_handler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiService::ip_event_handler, this));
}

void WifiService::wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    WifiService* self = reinterpret_cast<WifiService*>(arg);
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WIFI_EVENT_STA_START");
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED");
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)event_data;
                ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED reason=%d", d ? d->reason : -1);
                self->wifi_connected = false;
                if (self->status_callback) self->status_callback(0); // DISCONNECTED
                // do not auto reconnect here aggressively — let NetworkManager decide
                break;
            }
            default:
                break;
        }
    }
}

void WifiService::ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    WifiService* self = reinterpret_cast<WifiService*>(arg);
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "GOT IP: " IPSTR, IP2STR(&event->ip_info.ip));
        self->wifi_connected = true;
        if (self->status_callback) self->status_callback(2); // GOT_IP
    }
}
