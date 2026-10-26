#include <cstdio>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_now_remote.hpp"

static const char *TAG = "main";

static EspNowRemote s_remote;

static void on_state(bool heat_on, bool link_up)
{
    if (!link_up) {
        ESP_LOGW(TAG, "Link lost, last known heat %s", heat_on ? "ON" : "OFF");
        return;
    }
    ESP_LOGI(TAG, "Heat %s", heat_on ? "ON" : "OFF");
}

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(s_remote.begin(on_state));
}
