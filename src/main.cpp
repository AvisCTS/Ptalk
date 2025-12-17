#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "AppController.hpp"
#include "DeviceProfile.hpp"

static const char *TAG = "MAIN_TEST";

// Định nghĩa màu RGB565 cơ bản
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F
#define YELLOW  0xFFE0

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "App Main started");

    // Khởi tạo AppController
    auto& app = AppController::instance();

    if (!DeviceProfile::setup(app)) {
        ESP_LOGE(TAG, "DeviceProfile setup failed");
        return;
    }

    if (!app.init()) {
        ESP_LOGE(TAG, "AppController init failed");
        return;
    }

    // Bắt đầu AppController
    app.start();

}