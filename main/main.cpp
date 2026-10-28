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

// Status LED (blue LED on the SuperMini, active low): flashes while looking for the master,
// off while linked, solid on after the link has been lost. With LED_LATCH_LINK_LOSS the solid
// state stays on even after the link recovers, so a past drop is visible without the serial
// monitor; set it to 0 to turn the LED off again once the master is back.
#define LED_GPIO            GPIO_NUM_8
#define LED_ON_LEVEL        0
#define LED_BLINK_MS        250
#define LED_LATCH_LINK_LOSS 1

static const char *TAG = "main";

static EspNowRemote s_remote;
static esp_timer_handle_t s_failsafe_timer;
static esp_timer_handle_t s_blink_timer;

enum class LedMode { Blink, Off, Solid };
static volatile LedMode s_led_mode = LedMode::Blink;
static bool s_link_was_lost = false;

static void led_write(bool on)
{
    gpio_set_level(LED_GPIO, on ? LED_ON_LEVEL : !LED_ON_LEVEL);
}

static void blink_cb(void *arg)
{
    static bool on = false;
    if (s_led_mode != LedMode::Blink) {
        return;
    }
    on = !on;
    led_write(on);
}

static void led_set_mode(LedMode mode)
{
    s_led_mode = mode;
    if (mode == LedMode::Blink) {
        esp_timer_stop(s_blink_timer);
        esp_timer_start_periodic(s_blink_timer, (uint64_t)LED_BLINK_MS * 1000);
        return;
    }
    esp_timer_stop(s_blink_timer);
    led_write(mode == LedMode::Solid);
}

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
        s_link_was_lost = true;
        led_set_mode(LedMode::Solid);
        return;
    }
    esp_timer_stop(s_failsafe_timer);
    led_set_mode((LED_LATCH_LINK_LOSS && s_link_was_lost) ? LedMode::Solid : LedMode::Off);
    ESP_LOGI(TAG, "Heat %s", heat_on ? "ON" : "OFF");
    relay_set(heat_on);
}

extern "C" void app_main(void)
{
    // Relay off before anything else; the level is latched before the pin becomes an output
    gpio_reset_pin(RELAY_GPIO);
    gpio_set_level(RELAY_GPIO, !RELAY_ON_LEVEL);
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);

    gpio_reset_pin(LED_GPIO);
    gpio_set_level(LED_GPIO, !LED_ON_LEVEL);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    const esp_timer_create_args_t blink_args = {
        .callback = blink_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "blink",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&blink_args, &s_blink_timer));
    led_set_mode(LedMode::Blink);

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
