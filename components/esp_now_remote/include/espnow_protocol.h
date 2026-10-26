#pragma once

// Wire protocol shared by the thermostat (master) and its remote.
// Copy this file unchanged into the remote project.

#include <stdint.h>

#define ESPNOW_PROTO_MAGIC    0x5448   // "TH"
#define ESPNOW_PROTO_VERSION  1

typedef enum {
    // Remote -> broadcast. Announces the remote so the master can pair with it.
    // Repeat it until the first ESPNOW_MSG_STATE arrives, and again whenever
    // the heartbeats stop.
    ESPNOW_MSG_HELLO = 1,
    // Master -> remote (unicast). Sent every few seconds; doubles as the heartbeat.
    ESPNOW_MSG_STATE = 2,
} espnow_msg_type_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;     // ESPNOW_PROTO_MAGIC
    uint8_t  version;   // ESPNOW_PROTO_VERSION
    uint8_t  type;      // espnow_msg_type_t
} espnow_header_t;

typedef struct __attribute__((packed)) {
    espnow_header_t header;
    uint8_t  on;        // 1 = heat ON, 0 = heat OFF
    uint8_t  seq;       // increments with every message, wraps at 255
} espnow_state_msg_t;

typedef struct __attribute__((packed)) {
    espnow_header_t header;
} espnow_hello_msg_t;
