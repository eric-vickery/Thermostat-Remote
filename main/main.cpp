#include <cstdio>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_now_remote.hpp"

// Relay output: driven RELAY_ON_LEVEL while the heat is on, the opposite level otherwise
#define RELAY_GPIO          GPIO_NUM_0
#define RELAY_ON_LEVEL      1

// The remote reports the link lost after EspNowRemote::LINK_TIMEOUT_MS of silence and then
// scans for the master. When the master changes WiFi channel the link is lost for that
// timeout plus the scan time. The relay is only forced off if the link is still down this
// long after it was reported lost, so a channel change does not trip the failsafe.
#define FAILSAFE_DELAY_MS   30000

static const char *TAG = "main";

static EspNowRemote s_remote;
static esp_timer_handle_t s_failsafe_timer;

static void relay_set(bool on)
{
    gpio_set_level(RELAY_GPIO, on ? RELAY_ON_LEVEL : !RELAY_ON_LEVEL);
}

static void failsafe_cb(void *arg)
{
    // The link may have come back just as the timer fired; link_up is set before the callback runs
    if (s_remote.link_up()) {
        return;
    }
    ESP_LOGW(TAG, "Link still down after %d ms, failsafe: heat OFF", FAILSAFE_DELAY_MS);
    relay_set(false);
}

static void on_state(bool heat_on, bool link_up)
{
    if (!link_up) {
        ESP_LOGW(TAG, "Link lost, holding heat %s for up to %d ms", heat_on ? "ON" : "OFF", FAILSAFE_DELAY_MS);
        esp_timer_stop(s_failsafe_timer);
        esp_timer_start_once(s_failsafe_timer, (uint64_t)FAILSAFE_DELAY_MS * 1000);
        return;
    }
    esp_timer_stop(s_failsafe_timer);
    ESP_LOGI(TAG, "Heat %s", heat_on ? "ON" : "OFF");
    relay_set(heat_on);
}

extern "C" void app_main(void)
{
    // Relay off before anything else; the level is latched before the pin becomes an output
    gpio_reset_pin(RELAY_GPIO);
    gpio_set_level(RELAY_GPIO, !RELAY_ON_LEVEL);
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);

    const esp_timer_create_args_t timer_args = {
        .callback = failsafe_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "failsafe",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_failsafe_timer));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(s_remote.begin(on_state));
}
