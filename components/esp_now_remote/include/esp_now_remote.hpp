#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "espnow_protocol.h"

// ESP-NOW remote for the thermostat. Receives the heater ON/OFF state that the
// master sends every few seconds.
//
// The remote does not know the master's WiFi channel (the master follows the
// router's channel), so until it hears from the master it hops through the
// channels broadcasting a hello on each one. The master pairs with the sender
// of the hello and answers with a state message right away, which locks the
// remote onto that channel. If no message arrives for LINK_TIMEOUT_MS the link
// is considered lost and the scan starts again, so a master that moved to a new
// channel is found automatically.
class EspNowRemote {
public:
    static constexpr uint8_t  MAX_CHANNEL      = 13;
    static constexpr uint32_t SCAN_DWELL_MS    = 150;    // time spent listening on each channel while scanning
    static constexpr uint32_t LINK_TIMEOUT_MS  = 10000;  // master sends every 3 s, so this is ~3 missed messages

    // Called from the remote's own task (not the WiFi task) when the link comes up,
    // when the heater state changes, and when the link is lost. On a lost link
    // heat_on holds the last known state; what to do about it is up to the application.
    using StateCallback = void (*)(bool heat_on, bool link_up);

    // init_wifi = true brings up netif, the event loop and WiFi in STA mode itself.
    // Pass false if the application already started WiFi. NVS must be initialised
    // by the application either way. Call once.
    esp_err_t begin(StateCallback callback, bool init_wifi = true);

    bool link_up() const { return _linked; }
    bool heat_on() const { return _heat_on; }

private:
    struct RxMsg {
        uint8_t src[6];
        uint8_t on;
        uint8_t seq;
    };

    static void task_entry_(void *arg);
    static void recv_cb_(const esp_now_recv_info_t *info, const uint8_t *data, int len);

    void task_();
    void handle_state_(const RxMsg &msg);
    void send_hello_();
    esp_err_t init_wifi_();

    StateCallback _callback = nullptr;
    QueueHandle_t _rx_queue = nullptr;
    uint8_t _master_mac[6] = {};
    volatile bool _linked = false;
    volatile bool _heat_on = false;

    static EspNowRemote *s_instance;
};
