# LoRa Basics Modem for ESP32 (ESP-IDF, SX126x)

A port of Semtech's LoRa Basics Modem (LBM, repository `Lora-net/SWL2001`) to ESP-IDF, for an ESP32 driving an SX1261 / SX1262 / SX1268 over SPI. LBM is Semtech's LoRaWAN stack for its transceivers (Class A/B/C, LoRaWAN 1.0.x and 1.1, regional parameters RP2 1.0.3 by default).

> **Status: builds with ESP-IDF 5.5.2 for esp32s3 (reported by a user); not yet run on hardware.** It also passes its tests on a host with mock ESP-IDF headers, and it has Heltec board presets. See [What was checked](#what-was-checked) and [Heltec boards](#heltec-boards) before relying on it.

## Known pitfalls

- Run `idf.py set-target esp32s3` (or choose it in the VS Code extension) before the first build. After changing the target, delete `build/` and `sdkconfig`.
- The component's `CMakeLists.txt` returns early during ESP-IDF's dependency scan (`CMAKE_BUILD_EARLY_EXPANSION`), because `CONFIG_*` options are not defined then. Older copies without that block fail with "enable at least one region".

## Layout

```
components/lora_basics_modem/
  Kconfig, CMakeLists.txt        radio, pins, regions, features, runtime options
  include/lbm_esp32.h            the API you call
  src/lbm_esp32.c                board bring-up, modem task, interrupt dispatch
  src/smtc_modem_hal_esp32.c     LBM HAL: time, timer, NVS storage, RNG, crash log
  src/sx126x_hal_esp32.c         radio SPI/BUSY/reset/wake
  src/ral_sx126x_bsp_esp32.c     PA, TCXO, regulator, DIO2 switch from Kconfig
  third_party/SWL2001/           unmodified upstream lbm_lib (commit in UPSTREAM_COMMIT)
examples/periodic_uplink/        OTAA join, then a counter uplink every N seconds
examples/otaa_class_a/           OTAA Class A with ADR profile, downlink commands, link check, status LED
tools/host_check/                host-side build and tests against mock ESP-IDF headers
```

The upstream sources are not modified. The component's `CMakeLists.txt` compiles them directly instead of calling upstream's CMake, which defines its own `project()` and `install()` rules.

## Quick start

1. Copy `components/lora_basics_modem` into your project's `components/` (or add this repo's `components` directory to `EXTRA_COMPONENT_DIRS`).
2. `idf.py menuconfig` → **LoRa Basics Modem**:
   - **Board preset**: pick your Heltec board (see [Heltec boards](#heltec-boards)), or Custom.
   - **Pins and SPI**: for a custom board, set MOSI, MISO, SCLK, NSS, NRESET, BUSY, DIO1. The defaults are the pins shared by the Heltec ESP32-S3 + SX1262 boards.
   - **Radio**: pick SX1262 / SX1268 / SX1261. Turn on TCXO only if the module has a TCXO fed from DIO3, and set its voltage. Leave DIO2-as-RF-switch on unless your module needs RXEN/TXEN from the ESP32.
   - **Stack features**: enable your region (EU868 and US915 are on by default) and Class B/C/CSMA if needed.
3. Put the following in `sdkconfig.defaults`:
   ```
   CONFIG_FREERTOS_HZ=1000
   CONFIG_ESP_TIMER_SUPPORT_ISR_DISPATCH_METHOD=y
   ```
4. In `app_main`, initialise NVS, then start the stack:
   ```c
   ESP_ERROR_CHECK(nvs_flash_init());          // with the usual erase-and-retry boilerplate
   ESP_ERROR_CHECK(lbm_esp32_start(NULL, my_event_callback));
   ```
   `my_event_callback` handles `SMTC_MODEM_EVENT_RESET` by setting credentials, region, and calling `smtc_modem_join_network()`. `examples/periodic_uplink/main/main.c` is a complete version.

To run the example: `cd examples/periodic_uplink && idf.py set-target esp32s3 && idf.py menuconfig` (set the **LoRaWAN example** credentials and the component's pins), then `idf.py build flash monitor`.

### Credentials

LBM names the keys by their LoRaWAN 1.1 role. For a **1.0.x** network server (including a TTN 1.0.x device), pass the device's AppKey to `smtc_modem_set_nwkkey()`. `smtc_modem_set_appkey()` takes the 1.1 AppKey (the "Gen AppKey" in 1.0.x), used for multicast key derivation.

## Heltec boards

Pick the board under **LoRa Basics Modem → Board preset**. A preset only changes defaults (TCXO on at 1.8 V with 5 ms start-up, DC-DC, DIO2 as RF switch, front-end pins); you can still edit every value.

| Board | Preset | Front-end pins the port drives |
|---|---|---|
| WiFi LoRa 32 V3, Wireless Stick Lite V3, Wireless Tracker, Wireless Paper | Heltec V3 | none |
| WiFi LoRa 32 V4.2 (GC1109) | Heltec V4.2 | GPIO7 high, GPIO2 high, GPIO46 high, GPIO36 low (Vext) |
| WiFi LoRa 32 V4.3 (KCT8103L) | Heltec V4.3 | GPIO7 high, GPIO2 high, GPIO36 low (Vext); GPIO5 (CTX) set high on transmit, low on receive |

All of them use NSS 8, SCK 9, MOSI 10, MISO 11, RESET 12, BUSY 13, DIO1 14 on an ESP32-S3 with an SX1262.

Where these values come from: the V3 pins, DIO2 switch and 1.8 V TCXO are from the [ESPHome Heltec V3 page](https://devices.esphome.io/devices/heltec-wifi-lora-32-v3/) and the Meshtastic firmware `variant.h` files for the Heltec V3, V4, Wireless Tracker and Wireless Paper. The V4 front-end pins are from Meshtastic's V4 variants only; I did not have the Heltec V4 schematic.

Things to check on your board:

- **Nothing has run on a Heltec board.** The presets are tested only in that the right values reach the right GPIOs and radio calls on the host.
- **V4 transmit power.** The V4 front-end adds gain after the SX1262, so the antenna power is higher than what the stack asks for. Set **Board TX power offset** negative to stay within your region's limit. Measure it; I have no figure.
- **V4 Vext (GPIO36, active low).** Meshtastic comments say it powers the OLED and the LoRa antenna boost. If your board's radio works with it left alone, you can set the pin to -1.
- **Which V4 revision you have.** V4.2 and V4.3 differ in the front-end chip and which pin does what. Using the wrong preset leaves the PA path or RX path unpowered. Check the silkscreen or Heltec's pin map.
- **WiFi LoRa 32 V1 and V2 are not supported.** They use an SX1276 on an original ESP32, and this port is SX126x only.
- **Wireless Tracker / Wireless Paper:** the radio pins match, but check that nothing else in your project uses GPIO 8-14.

## Threading rules

All stack calls (`smtc_modem_*`) must be serialised:

- The event callback runs in the modem task with the stack lock already held. Call `smtc_modem_*` directly and don't block.
- From any other task, wrap calls in `lbm_esp32_lock()` / `lbm_esp32_unlock()`, then call `lbm_esp32_wake()` so the engine runs.

## How the interrupt handling works

LBM is written for bare metal: its timer and radio callbacks run in interrupt context, must be atomic with respect to the main loop (`smtc_modem_hal_disable/enable_modem_irq` bracket its critical sections), and the radio callback timestamps the event, which schedules the RX1/RX2 windows.

Here, the real ISRs (DIO1 and an `esp_timer` in ISR-dispatch mode) only record a pending bit and notify the `lbm_irq` task. That task runs above the modem task's priority and is pinned to the same core, so it preempts the engine immediately, which keeps the timestamp accurate. Because it shares a core with the engine, it cannot run while the engine is inside a masked section. Masking is a flag; events that arrive while masked replay on unmask. The stack's own callbacks (which live in flash) never run from an ISR, so a flash write cannot fault an interrupt.

## What was checked

Checked here, on a Linux host:

- `tools/host_check/run.sh` builds the component from its own `CMakeLists.txt` against mock ESP-IDF headers in seven configurations (defaults, with `esp_timer` ISR dispatch off, everything on including SX1261 + LDO + TCXO + RXEN/TXEN + all regions + Class B/C + CSMA, SX1268 with trace off, and the three Heltec presets). The port is compiled with `-Wall -Wextra -Werror`, and everything is linked with `--whole-archive`, so any HAL function the stack needs but the port lacks shows up as an undefined symbol.
- The Kconfig files parse with Espressif's `kconfgen`.
- The host tests run the port's own logic: SPI framing for reads and writes, NSS handling, wake-from-sleep, BUSY timeout, RXEN/TXEN switching by opcode, the BSP values (TCXO voltage and ticks, regulator, RF switch, PA settings, power clamp), the Heltec preset pins and fixed-level GPIOs after start-up, NVS context store/restore (offsets, skip-if-unchanged, missing data), 32-bit millisecond wrap, random range, crash log, timer and radio dispatch (mask, replay, stop-cancels-pending).
- The real LBM stack initialises on a fake radio (all SPI reads return zero), runs one engine pass, and delivers `SMTC_MODEM_EVENT_RESET`.

**Not checked:**

- Only one real build so far: ESP-IDF 5.5.2, esp32s3, built by a user on Windows. Other IDF versions and targets are untested. Written for IDF 5.x; IDF 4.4 untested.
- No hardware run: no join, no RX window timing, no TXEN/RXEN, no TCXO start.
- Only ESP32 / ESP32-S3 style targets were considered. Single-core and RISC-V chips are untested.

First things to verify on hardware: the join completes; an uplink reaches the gateway; downlinks in RX1 are received (if not, raise `LBM_BOARD_DELAY_MS`, and look at RX window timing with a logic analyser on DIO1).

## Limitations

- SX126x only (SX1261/1262/1268). SX127x, SX128x and LR11xx would need their own HAL and BSP.
- Not provided: FUOTA, Store-and-Forward, Relay, Beacon TX, geolocation, multicast, cloud services. The HAL has inert stubs for the FUOTA/S&F hooks. Enabling these upstream options needs flash-page storage and more porting work.
- No light sleep or power management yet. The modem task waits on a FreeRTOS notification, but nothing gates sleep around the stack's timers.
- Context storage uses NVS, one blob per context type, and skips writes when nothing changed. An NVS page rotation can erase a flash sector and stall every task for tens of milliseconds. If one lands between an uplink and RX1, that window can be missed. Check this on hardware if you see occasional lost downlinks.
- `esp_random()` is a true random source only with Wi-Fi/Bluetooth on or `bootloader_random_enable()` active. LBM uses it for jitter and back-off, not key material, so this is a quality note, not a security hole.
- RXEN/TXEN switching is driven by the SetRx/SetTx opcodes sent to the radio. It is untested; TXEN stays asserted after a transmit until the next command.
- AES and CMAC use the stack's software secure element. The ESP32 hardware AES is not used.
- Trace output goes through `vprintf` on the main console and slows the engine slightly. Turn it off in menuconfig when timing-testing.

## Updating the upstream library

Replace `components/lora_basics_modem/third_party/SWL2001/lbm_lib` with a newer checkout and update `UPSTREAM_COMMIT`. If upstream adds source files, mirror them in the lists in the component's `CMakeLists.txt` (compare with `lbm_lib/smtc_modem_core/CMakeLists.txt`). Run `tools/host_check/run.sh` after the update; undefined-symbol errors there point to new HAL functions to implement.

## License

Port code is derived from Semtech's reference HAL and BSP files and carries their notice. The upstream library is under the Clear BSD License (see `third_party/SWL2001/LICENSE.txt`).
