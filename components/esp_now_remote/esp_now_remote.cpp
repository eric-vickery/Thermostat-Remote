#include "esp_now_remote.hpp"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "Thermostat_Remote";

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

EspNowRemote *EspNowRemote::s_instance = nullptr;

esp_err_t EspNowRemote::begin(StateCallback callback, bool init_wifi)
{
    if (s_instance) {
        return ESP_ERR_INVALID_STATE;
    }
    s_instance = this;
    _callback = callback;

    if (init_wifi) {
        esp_err_t err = init_wifi_();
        if (err != ESP_OK) {
            s_instance = nullptr;
            return err;
        }
    }
    // Modem sleep makes the radio miss the master's unicast messages
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        s_instance = nullptr;
        return err;
    }
    esp_now_register_recv_cb(recv_cb_);

    // Peer for the broadcast hello; channel 0 = whatever channel the radio is on
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add broadcast peer: %s", esp_err_to_name(err));
        return err;
    }

    _rx_queue = xQueueCreate(4, sizeof(RxMsg));
    if (xTaskCreate(task_entry_, "espnow_remote", 4096, this, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Started, scanning for master");
    return ESP_OK;
}

esp_err_t EspNowRemote::init_wifi_()
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    // Not connecting to any router, so nothing to keep in flash
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    return esp_wifi_start();
}

// Runs in the WiFi task, so only validate and hand off
void EspNowRemote::recv_cb_(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!s_instance || !info || !info->src_addr || len != (int)sizeof(espnow_state_msg_t)) {
        return;
    }
    const espnow_state_msg_t *m = (const espnow_state_msg_t *)data;
    if (m->header.magic != ESPNOW_PROTO_MAGIC || m->header.version != ESPNOW_PROTO_VERSION ||
        m->header.type != ESPNOW_MSG_STATE) {
        return;
    }
    RxMsg msg;
    memcpy(msg.src, info->src_addr, 6);
    msg.on = m->on;
    msg.seq = m->seq;
    msg.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    xQueueSend(s_instance->_rx_queue, &msg, 0);
}

void EspNowRemote::task_entry_(void *arg)
{
    static_cast<EspNowRemote *>(arg)->task_();
}

void EspNowRemote::task_()
{
    RxMsg msg;
    uint8_t channel = 1;
    while (true) {
        if (!_linked) {
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
            send_hello_();
            if (xQueueReceive(_rx_queue, &msg, pdMS_TO_TICKS(SCAN_DWELL_MS)) == pdTRUE) {
                handle_state_(msg);
                ESP_LOGI(TAG, "Master found on channel %u", (unsigned)channel);
            } else {
                channel = (channel >= MAX_CHANNEL) ? 1 : channel + 1;
            }
        } else {
            if (xQueueReceive(_rx_queue, &msg, pdMS_TO_TICKS(LINK_TIMEOUT_MS)) == pdTRUE) {
                handle_state_(msg);
            } else {
                ESP_LOGW(TAG, "No message from master for %lu ms, link lost, scanning",
                         (unsigned long)LINK_TIMEOUT_MS);
                _linked = false;
                if (_callback) {
                    _callback(_heat_on, false);
                }
            }
        }
    }
}

void EspNowRemote::handle_state_(const RxMsg &msg)
{
    bool on = msg.on != 0;
    int64_t now_us = esp_timer_get_time();

    if (!_linked) {
        memcpy(_master_mac, msg.src, 6);
        _last_rx_us = now_us;
        _linked = true;
        _heat_on = on;
        ESP_LOGI(TAG, "Linked to master " MACSTR ", heat %s", MAC2STR(_master_mac), on ? "ON" : "OFF");
        if (_callback) {
            _callback(on, true);
        }
        return;
    }
    if (memcmp(msg.src, _master_mac, 6) != 0) {
        ESP_LOGW(TAG, "Ignoring state from " MACSTR ", master is " MACSTR,
                 MAC2STR(msg.src), MAC2STR(_master_mac));
        return;
    }
    ESP_LOGI(TAG, "Message from master: heat %s, seq %u, %lu ms since last, RSSI %d dBm",
             on ? "ON" : "OFF", (unsigned)msg.seq,
             (unsigned long)((now_us - _last_rx_us) / 1000), (int)msg.rssi);
    _last_rx_us = now_us;
    if (on != _heat_on) {
        _heat_on = on;
        ESP_LOGI(TAG, "Heat %s (seq %u)", on ? "ON" : "OFF", (unsigned)msg.seq);
        if (_callback) {
            _callback(on, true);
        }
    }
}

void EspNowRemote::send_hello_()
{
    espnow_hello_msg_t hello = {};
    hello.header.magic = ESPNOW_PROTO_MAGIC;
    hello.header.version = ESPNOW_PROTO_VERSION;
    hello.header.type = ESPNOW_MSG_HELLO;
    esp_err_t err = esp_now_send(BROADCAST_MAC, (const uint8_t *)&hello, sizeof(hello));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Hello send failed: %s", esp_err_to_name(err));
    }
}
