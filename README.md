# Thermostat Remote

ESP32-C3 firmware for a remote unit that mirrors the heater state of a WiFi thermostat. The thermostat (the *master*) broadcasts whether the heater is ON or OFF over ESP-NOW. This remote finds the master automatically, follows its state, and tells the application when the link comes up, the state changes, or the link is lost.

Built with ESP-IDF 6.1 for the ESP32-C3 (developed on the ESP32-C3 SuperMini).

## Features

- **No channel configuration.** The master follows the router's WiFi channel, which can change. The remote finds it by itself.
- **Automatic discovery.** Until it hears from the master, the remote hops through channels 1 to 13 and broadcasts a hello on each one, dwelling 150 ms per channel.
- **Self-healing link.** The master sends a state message every few seconds, which doubles as a heartbeat. If nothing arrives for 10 seconds, the link is declared lost and the scan restarts, so a master that moves to another channel is picked up again.
- **Simple callback API.** One function is called when the link comes up, when the heater state changes, and when the link is lost.
- **No router credentials.** The remote never joins a WiFi network. WiFi runs in station mode purely as the ESP-NOW radio.
- **Relay output.** GPIO0 follows the heater state: high when the heat is on, low when it is off. It is driven low as the first thing at startup.
- **Failsafe.** If the link stays down past a grace period, the relay is forced off. The grace period is long enough that a master changing WiFi channel does not trip it.
- **Status LED.** The onboard blue LED shows the link state without a serial monitor, and a drop leaves it lit as a record.
- **Link diagnostics.** Every message from the master is logged with its sequence number, the time since the previous message, and RSSI, so link stability is visible in the monitor.

## How it works

```
 Remote                                   Master (thermostat)
   |  -- HELLO (broadcast, ch N) -------->  |
   |  -- HELLO (broadcast, ch N+1) ------>  |   remote hops channels
   |  -- HELLO (broadcast, ch M) -------->  |   until the master hears it
   |  <------------- STATE (unicast) -----  |   master pairs, answers at once
   |                                        |   remote locks onto channel M
   |  <------------- STATE every ~3 s ----  |   heartbeat + heater state
   |                 ... silence 10 s ...   |
   |  link lost, scan restarts              |
```

### Wire protocol

Defined in [`espnow_protocol.h`](components/esp_now_remote/include/espnow_protocol.h). This file is shared with the master, and the copy in the master project must stay in step with this one.

| Message | Direction | Delivery | Payload |
|---|---|---|---|
| `ESPNOW_MSG_HELLO` | Remote to master | Broadcast | Header only |
| `ESPNOW_MSG_STATE` | Master to remote | Unicast, every few seconds | `on` (1 = heat ON), `seq` (wraps at 255) |

Every message starts with a 4-byte header: magic `0x5448` ("TH"), protocol version, and message type. Messages with a wrong magic, version, type, or length are dropped.

Once linked, the remote accepts state only from the master it first linked to and ignores messages from any other sender.

## Project layout

```
.
├── CMakeLists.txt
├── main/
│   └── main.cpp                      # relay, failsafe, status LED, starts the remote
└── components/
    └── esp_now_remote/
        ├── esp_now_remote.cpp        # scan, link tracking, ESP-NOW handling
        └── include/
            ├── esp_now_remote.hpp    # EspNowRemote class
            └── espnow_protocol.h     # wire protocol shared with the master
```

## Usage

The minimal pattern for using the component is below. [`main.cpp`](main/main.cpp) is the full application, with the relay, failsafe, and LED added to the callback.

```cpp
#include "nvs_flash.h"
#include "esp_now_remote.hpp"

static EspNowRemote s_remote;

static void on_state(bool heat_on, bool link_up)
{
    if (!link_up) {
        // Link lost; heat_on holds the last known state
        return;
    }
    // Drive an output, LED, display, etc. from heat_on
}

extern "C" void app_main(void)
{
    // NVS must be initialised by the application (see main.cpp)
    ESP_ERROR_CHECK(s_remote.begin(on_state));
}
```

`begin(callback, init_wifi = true)` starts the remote. Pass `init_wifi = false` if your application has already started WiFi.

| Member | Description |
|---|---|
| `link_up()` | `true` while heartbeats are arriving |
| `heat_on()` | Last heater state received |

The callback runs on the remote's own task, not the WiFi task, so it is safe to do normal work in it. What to do when the link is lost is up to the application. For example, you might turn an output off or hold the last state.

## Relay, failsafe and status LED

All of this lives in [`main.cpp`](main/main.cpp), driven from the remote's state callback.

### Relay output

GPIO0 (`RELAY_GPIO`) is driven to `RELAY_ON_LEVEL` while the heat is on. At startup it is set to the off level before the pin becomes an output, and it stays off until the first link is established.

Firmware cannot control the pin before `app_main` runs (the ROM and bootloader take a few hundred milliseconds), so **add a pull-down of about 10k from GPIO0 to GND** to keep the relay off from power-up and through resets.

### Failsafe

When the link is lost (no message for `LINK_TIMEOUT_MS`), the relay holds its last state and a timer starts. If the link has not recovered after `FAILSAFE_DELAY_MS` (30 s), the relay is forced off. If the master comes back first, the timer is cancelled and the current state is applied.

A master changing WiFi channel costs the remote about `LINK_TIMEOUT_MS` plus a couple of seconds of scanning, which is well inside the grace period. In total the heat goes off roughly 40 s after the last message from a master that has really gone away.

### Status LED

The blue LED on GPIO8 (active low) shows the link state:

| LED | Meaning |
|---|---|
| Flashing | Scanning for the master (boot) |
| Off | Linked |
| Solid on | The link has been lost |

With `LED_LATCH_LINK_LOSS` set to 1 (the default), the solid state stays on after the link recovers. A past drop stays visible until the next reboot. Set it to 0 to turn the LED off again when the master returns.

### Configuration

| Define | Default | Meaning |
|---|---|---|
| `RELAY_GPIO` | `GPIO_NUM_0` | Relay output pin |
| `RELAY_ON_LEVEL` | 1 | Pin level that turns the relay on (0 for active-low modules) |
| `FAILSAFE_DELAY_MS` | 30000 | Time after the link is reported lost before the relay is forced off |
| `LED_GPIO` | `GPIO_NUM_8` | Status LED pin |
| `LED_ON_LEVEL` | 0 | Pin level that lights the LED |
| `LED_BLINK_MS` | 250 | Flash half-period while scanning |
| `LED_LATCH_LINK_LOSS` | 1 | Keep the LED on after a link drop recovers |

### Hardware notes

- The relay input is driven through the module's optocoupler (an EL817 in the current build), so GPIO0 only has to supply the LED current through its resistor.
- A 3.3 V relay coil drawing around 120 mA plus the ESP32-C3's WiFi bursts (up to 300 to 350 mA) puts the SuperMini's onboard regulator close to its limit. Add a 470 to 1000 µF capacitor across 3V3 and GND near the relay, or power the coil from a separate supply with the grounds tied together.

## Building and flashing

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/) 6.1 with the environment exported.

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

### ESP32-C3 SuperMini notes

- The board uses the chip's native USB, so set the console to **USB Serial/JTAG** in `menuconfig`: `Component config > ESP System Settings > Channel for console output`.
- **GPIO9** is the BOOT button and a boot-strapping pin. GPIO2 and GPIO8 are strapping pins too. Don't hang pulldowns on them, or the chip can boot into download mode ("waiting for download").
- If the monitor drops the chip into download mode when it opens, use `idf.py monitor --no-reset`, or press the RESET button.

### Link logging

While linked, each message from the master produces a line like:

```
Message from master: heat ON, seq 42, 3012 ms since last, RSSI -58 dBm
```

Gaps close to the master's heartbeat interval and consecutive sequence numbers mean a stable link. A gap approaching `LINK_TIMEOUT_MS` or a jump in `seq` means messages are being missed. RSSI around -80 dBm or worse is marginal.

## Configuration

Timing constants live in [`esp_now_remote.hpp`](components/esp_now_remote/include/esp_now_remote.hpp):

| Constant | Default | Meaning |
|---|---|---|
| `MAX_CHANNEL` | 13 | Highest WiFi channel scanned |
| `SCAN_DWELL_MS` | 150 | Time spent on each channel while scanning |
| `LINK_TIMEOUT_MS` | 10000 | Silence before the link is considered lost |

If the master's heartbeat interval changes, keep `LINK_TIMEOUT_MS` at roughly three times it.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Stuck at `Started, scanning for master` | The master isn't powered, or it runs a different protocol version. Check the `espnow_protocol.h` copies. Channels 12 and 13 may also be unavailable in some regions. |
| Link drops repeatedly | The master's heartbeat is slower than `LINK_TIMEOUT_MS`, or the signal is marginal. |
| No logs on the serial monitor | Console output is not set to USB Serial/JTAG. |
| `waiting for download` | GPIO9 held low at reset. See the SuperMini notes above. |
| Relay clicks or board resets when the heat comes on | The 3V3 regulator is overloaded by the coil. See the hardware notes above. |
| LED solid on although the master is back | `LED_LATCH_LINK_LOSS` is 1; reboot to clear it, or set it to 0. |

## License

Released under the [MIT License](LICENSE).
