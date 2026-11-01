/*
 * ESP32 runtime for Semtech LoRa Basics Modem (SX126x).
 *
 * Owns the radio SPI/GPIO setup, the modem task and the interrupt dispatch task.
 * The stack itself (smtc_modem_*) is declared in smtc_modem_api.h.
 *
 * Threading rules
 *   - The event callback passed to lbm_esp32_start() runs in the modem task with
 *     the stack lock held. Call smtc_modem_* directly from it and do not block.
 *   - From any other task, wrap every smtc_modem_* call in lbm_esp32_lock() /
 *     lbm_esp32_unlock(), then call lbm_esp32_wake() so the engine runs.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      spi_host;          /* spi_host_device_t, e.g. SPI2_HOST */
    int      pin_mosi;
    int      pin_miso;
    int      pin_sclk;
    int      pin_nss;
    int      pin_nrst;
    int      pin_busy;
    int      pin_dio1;          /* radio IRQ line */
    int      pin_rxen;          /* -1 if unused */
    int      pin_txen;          /* -1 if unused */
    int      fixed_pin[4];      /* outputs held at fixed_level[i] from start-up; -1 if unused */
    int      fixed_level[4];
    uint32_t spi_clock_hz;
    int      task_core;         /* modem and IRQ tasks share this core */
    int      task_priority;     /* modem task priority */
    uint32_t task_stack_bytes;  /* modem task stack */
} lbm_esp32_config_t;

/** Configuration built from menuconfig (Kconfig) values. */
lbm_esp32_config_t lbm_esp32_default_config(void);

/**
 * Initialise NVS access, the radio bus, the stack, and start the modem task.
 *
 * nvs_flash_init() must succeed first (the stack stores its context in NVS).
 * Pass NULL for cfg to use lbm_esp32_default_config(). event_cb is the stack's
 * event callback (it receives SMTC_MODEM_EVENT_RESET on the first engine run).
 */
esp_err_t lbm_esp32_start(const lbm_esp32_config_t *cfg, void (*event_cb)(void));

/** Take / release the stack lock (recursive). */
void lbm_esp32_lock(void);
void lbm_esp32_unlock(void);

/** Wake the modem task so it runs the engine now. Safe from any task. */
void lbm_esp32_wake(void);

#ifdef __cplusplus
}
#endif
