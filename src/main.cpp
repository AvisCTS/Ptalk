#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "../lib/wifi/WifiManager.hpp"

static const char* TAG = "APP_MAIN";

extern "C" void app_main()
{
    ESP_LOGI(TAG, "===== PTalk – WiFiManager + WebSocket Test =====");

    // Khởi tạo WiFi
    WifiManager::instance().init();
    while(!WifiManager::instance().isConnected()){
        WifiManager::instance().startCaptivePortal();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "WiFi connected ✔  IP: %s",
             WifiManager::instance().getIp().c_str());

    
    while (true)
    {
        ESP_LOGI(TAG, "App is running...");
        //ip
        ESP_LOGI(TAG, "IP Address: %s", WifiManager::instance().getIp().c_str());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
