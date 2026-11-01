# OTAA Class A example

A LoRaWAN Class A device that joins with OTAA and sends an uplink every N seconds. Uses the whole port: board presets, NVS session storage, the modem task and interrupt dispatch, and the stack's event API.

    idf.py set-target esp32s3
    idf.py menuconfig
    idf.py build flash monitor

In menuconfig:

- **LoRaWAN OTAA Class A example**: DevEUI, JoinEUI, AppKey, region, uplink period, FPort, confirmed uplinks, ADR profile, link check interval, status LED pin.
- **LoRa Basics Modem → Board preset**: Heltec V3 is the default (set in `sdkconfig.defaults`). Choose V4.2 or V4.3 for a WiFi LoRa 32 V4, and enable the same region under **Stack features**.

## Flow

| Event | What the example does |
|---|---|
| `RESET` | Sets DevEUI, JoinEUI, keys, region, selects Class A, starts the join |
| `JOINED` | Turns the LED on, applies the ADR profile, sends the first uplink, starts the alarm timer |
| `ALARM` | Sends an uplink, restarts the alarm timer |
| `TXDONE` | Logs whether the frame was sent, and whether the network acknowledged it (confirmed uplinks) |
| `DOWNDATA` | Logs window, RSSI, SNR, data rate and payload, then acts on the port (below) |
| `LINK_CHECK` | Logs margin and gateway count (only if enabled) |
| `JOINFAIL` | Logs it; the stack retries with back-off on its own |

Uplink payload (8 bytes, big endian): uplink counter, then seconds since boot. If the current data rate allows fewer bytes the example sends only the counter.

Class A means downlinks arrive only in the two receive windows after an uplink, so a downlink you schedule reaches the device after its next uplink.

## Downlink commands

| FPort | Payload | Effect |
|---|---|---|
| 2 | 1 byte, `00` or non-zero | LED off / on |
| 3 | 2 bytes, seconds, big endian | New uplink period (minimum 10 s) |

## Credentials

For a LoRaWAN 1.0.x network server (including a TTN 1.0.x device) enter the device's AppKey. The example passes it to both `smtc_modem_set_nwkkey()` and `smtc_modem_set_appkey()`.

## Checked

`main.c` compiles with `-Wall -Wextra -Werror` against mock ESP-IDF headers for two configurations (`tools/host_check/run.sh`). It has not been built with the ESP-IDF toolchain or run on a board.
