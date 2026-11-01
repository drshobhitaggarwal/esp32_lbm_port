/*!
 * \file      sx126x_hal_esp32.c
 *
 * \brief     SX126x radio driver HAL for ESP-IDF (SPI master, GPIO)
 *
 * Structure follows lbm_examples/radio_hal/sx126x_hal.c from Semtech's SWL2001
 * repository.
 *
 * The Clear BSD License
 * Copyright Semtech Corporation 2021. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted (subject to the limitations in the disclaimer
 * below) provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Semtech corporation nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY
 * THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT
 * NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SEMTECH CORPORATION BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "sx126x_hal.h"
#include "lbm_esp32_priv.h"

static const char *TAG = "sx126x_hal";

/* Largest SPI frame: WriteBuffer = opcode + offset + 255 payload bytes. */
#define SX126X_SPI_BUF_SIZE 264

/* Opcodes that decide the external RF switch state (RXEN/TXEN). */
#define SX126X_OP_SET_STANDBY            0x80
#define SX126X_OP_SET_RX                 0x82
#define SX126X_OP_SET_TX                 0x83
#define SX126X_OP_SET_SLEEP              0x84
#define SX126X_OP_SET_RX_DUTY_CYCLE      0x94
#define SX126X_OP_SET_CAD                0xC5
#define SX126X_OP_SET_FS                 0xC1
#define SX126X_OP_SET_TX_CONTINUOUS_WAVE 0xD1
#define SX126X_OP_SET_TX_INFINITE_PREAMBLE 0xD2

typedef enum {
    RADIO_SLEEP,
    RADIO_AWAKE,
} radio_sleep_mode_t;

static radio_sleep_mode_t s_radio_mode = RADIO_AWAKE;

static DMA_ATTR uint8_t s_tx[SX126X_SPI_BUF_SIZE];
static DMA_ATTR uint8_t s_rx[SX126X_SPI_BUF_SIZE];

static bool wait_on_busy(void)
{
    const int64_t timeout_us = (int64_t)CONFIG_LBM_BUSY_TIMEOUT_MS * 1000;
    const int64_t start      = esp_timer_get_time();
    while (gpio_get_level(g_lbm_cfg.pin_busy) == 1) {
        if (esp_timer_get_time() - start > timeout_us) {
            ESP_LOGE(TAG, "BUSY stuck high");
            return false;
        }
    }
    return true;
}

/* If the radio sleeps, wake it (NSS low until BUSY drops) before any transfer. */
static bool check_device_ready(void)
{
    if (s_radio_mode != RADIO_SLEEP) {
        return wait_on_busy();
    }
    gpio_set_level(g_lbm_cfg.pin_nss, 0);
    bool ok = wait_on_busy();
    gpio_set_level(g_lbm_cfg.pin_nss, 1);
    s_radio_mode = RADIO_AWAKE;
    return ok;
}

static void rf_switch_for_opcode(uint8_t opcode)
{
    const int rxen = g_lbm_cfg.pin_rxen;
    const int txen = g_lbm_cfg.pin_txen;
    if (rxen < 0 && txen < 0) {
        return;
    }
    int rx = -1;
    int tx = -1;
    switch (opcode) {
    case SX126X_OP_SET_TX:
    case SX126X_OP_SET_TX_CONTINUOUS_WAVE:
    case SX126X_OP_SET_TX_INFINITE_PREAMBLE:
        rx = 0;
        tx = 1;
        break;
    case SX126X_OP_SET_RX:
    case SX126X_OP_SET_RX_DUTY_CYCLE:
    case SX126X_OP_SET_CAD:
        rx = 1;
        tx = 0;
        break;
    case SX126X_OP_SET_STANDBY:
    case SX126X_OP_SET_FS:
    case SX126X_OP_SET_SLEEP:
        rx = 0;
        tx = 0;
        break;
    default:
        return;
    }
    if (rxen >= 0) {
        gpio_set_level(rxen, rx);
    }
    if (txen >= 0) {
        gpio_set_level(txen, tx);
    }
}

static esp_err_t spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_device_polling_transmit(g_lbm_spi, &t);
}

sx126x_hal_status_t sx126x_hal_write(const void *context, const uint8_t *command, const uint16_t command_length,
                                     const uint8_t *data, const uint16_t data_length)
{
    (void)context;
    const size_t total = (size_t)command_length + data_length;
    if (total > SX126X_SPI_BUF_SIZE || command_length == 0) {
        return SX126X_HAL_STATUS_ERROR;
    }
    if (!check_device_ready()) {
        return SX126X_HAL_STATUS_ERROR;
    }

    rf_switch_for_opcode(command[0]);

    memcpy(s_tx, command, command_length);
    if (data_length > 0) {
        memcpy(s_tx + command_length, data, data_length);
    }

    gpio_set_level(g_lbm_cfg.pin_nss, 0);
    esp_err_t err = spi_xfer(s_tx, NULL, total);
    gpio_set_level(g_lbm_cfg.pin_nss, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi write failed: %s", esp_err_to_name(err));
        return SX126X_HAL_STATUS_ERROR;
    }

    /* In sleep BUSY stays high, so do not wait on it after SetSleep. */
    if (command[0] == SX126X_OP_SET_SLEEP) {
        s_radio_mode = RADIO_SLEEP;
    } else if (!check_device_ready()) {
        return SX126X_HAL_STATUS_ERROR;
    }
    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_read(const void *context, const uint8_t *command, const uint16_t command_length,
                                    uint8_t *data, const uint16_t data_length)
{
    (void)context;
    const size_t total = (size_t)command_length + data_length;
    if (total > SX126X_SPI_BUF_SIZE || command_length == 0) {
        return SX126X_HAL_STATUS_ERROR;
    }
    if (!check_device_ready()) {
        return SX126X_HAL_STATUS_ERROR;
    }

    /* One full-duplex frame: command bytes (the driver already appended its NOPs),
     * then data_length clock-only bytes during which the radio shifts data out. */
    memcpy(s_tx, command, command_length);
    memset(s_tx + command_length, 0, data_length);

    gpio_set_level(g_lbm_cfg.pin_nss, 0);
    esp_err_t err = spi_xfer(s_tx, s_rx, total);
    gpio_set_level(g_lbm_cfg.pin_nss, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi read failed: %s", esp_err_to_name(err));
        return SX126X_HAL_STATUS_ERROR;
    }

    memcpy(data, s_rx + command_length, data_length);
    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_reset(const void *context)
{
    (void)context;
    gpio_set_level(g_lbm_cfg.pin_nrst, 0);
    esp_rom_delay_us(5000);
    gpio_set_level(g_lbm_cfg.pin_nrst, 1);
    esp_rom_delay_us(5000);
    s_radio_mode = RADIO_AWAKE;
    if (g_lbm_cfg.pin_rxen >= 0) {
        gpio_set_level(g_lbm_cfg.pin_rxen, 0);
    }
    if (g_lbm_cfg.pin_txen >= 0) {
        gpio_set_level(g_lbm_cfg.pin_txen, 0);
    }
    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_wakeup(const void *context)
{
    (void)context;
    return check_device_ready() ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}
