/*!
 * \file      smtc_modem_hal_esp32.c
 *
 * \brief     LoRa Basics Modem HAL for ESP-IDF (FreeRTOS, esp_timer, NVS)
 *
 * Structure follows the reference HAL in lbm_examples/smtc_modem_hal/smtc_modem_hal.c
 * from Semtech's SWL2001 repository.
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

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs.h"

#include "smtc_modem_hal.h"
#include "lbm_esp32_priv.h"

static const char *TAG = "lbm_hal";

#define LBM_NVS_NAMESPACE "lbm"
#define CRASHLOG_MAGIC    0xC0DE1B01u

/* ---------------------------------------------------------------- state ---- */

static uint32_t s_wrap_offset_ms; /* only used by the stack's wrap-around test */
static nvs_handle_t s_nvs;
static bool         s_nvs_open;

/* Survives a software reset; the magic guards against garbage after power-on. */
RTC_NOINIT_ATTR static uint32_t s_crash_magic;
RTC_NOINIT_ATTR static uint8_t  s_crash_buf[CRASH_LOG_SIZE];
RTC_NOINIT_ATTR static uint8_t  s_crash_len;
RTC_NOINIT_ATTR static uint8_t  s_crash_available;

/* ------------------------------------------------------ reset / watchdog --- */

void smtc_modem_hal_reset_mcu(void)
{
    esp_restart();
}

void smtc_modem_hal_reload_wdog(void)
{
    /* Only called by the stack's test code. Fails harmlessly if the calling
     * task is not subscribed to the task watchdog. */
    (void)esp_task_wdt_reset();
}

/* ------------------------------------------------------------------- time -- */

uint32_t smtc_modem_hal_get_time_in_s(void)
{
    return (uint32_t)((uint64_t)(esp_timer_get_time() / 1000000LL) + s_wrap_offset_ms / 1000u);
}

uint32_t smtc_modem_hal_get_time_in_ms(void)
{
    /* Truncating the 64-bit microsecond count gives the 0xFFFFFFFF -> 0 wrap the stack expects. */
    return (uint32_t)(esp_timer_get_time() / 1000LL) + s_wrap_offset_ms;
}

void smtc_modem_hal_set_offset_to_test_wrapping(const uint32_t offset_to_test_wrapping)
{
    s_wrap_offset_ms = offset_to_test_wrapping;
}

/* ------------------------------------------------------------------ timer -- */

void smtc_modem_hal_start_timer(const uint32_t milliseconds, void (*callback)(void *context), void *context)
{
    lbm_esp32_timer_start(milliseconds, callback, context);
}

void smtc_modem_hal_stop_timer(void)
{
    lbm_esp32_timer_stop();
}

/* -------------------------------------------------------------------- IRQ -- */

void smtc_modem_hal_disable_modem_irq(void)
{
    lbm_esp32_irq_mask(true);
}

void smtc_modem_hal_enable_modem_irq(void)
{
    lbm_esp32_irq_mask(false);
}

void smtc_modem_hal_irq_config_radio_irq(void (*callback)(void *context), void *context)
{
    lbm_esp32_set_radio_irq_cb(callback, context);
}

void smtc_modem_hal_user_lbm_irq(void)
{
    lbm_esp32_notify_modem_task();
}

/* --------------------------------------------------- context persistence --- */

static esp_err_t nvs_ensure_open(void)
{
    if (s_nvs_open) {
        return ESP_OK;
    }
    esp_err_t err = nvs_open(LBM_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err == ESP_OK) {
        s_nvs_open = true;
    }
    return err;
}

static void ctx_key(modem_context_type_t type, char key[8])
{
    snprintf(key, 8, "ctx%d", (int)type);
}

static bool ctx_type_supported(modem_context_type_t type)
{
    return type == CONTEXT_MODEM || type == CONTEXT_KEY_MODEM || type == CONTEXT_LORAWAN_STACK ||
           type == CONTEXT_SECURE_ELEMENT;
}

void smtc_modem_hal_context_restore(const modem_context_type_t ctx_type, uint32_t offset, uint8_t *buffer,
                                    const uint32_t size)
{
    if (!ctx_type_supported(ctx_type)) {
        smtc_modem_hal_on_panic((uint8_t *)__func__, __LINE__, "unsupported context type %d", (int)ctx_type);
        return;
    }
    /* Missing data reads as 0xFF, like erased flash on the reference board. */
    memset(buffer, 0xFF, size);

    char key[8];
    ctx_key(ctx_type, key);
    if (nvs_ensure_open() != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }

    size_t stored = 0;
    esp_err_t err = nvs_get_blob(s_nvs, key, NULL, &stored);
    if (err != ESP_OK || stored <= offset) {
        return; /* nothing stored (yet) */
    }
    uint8_t *tmp = malloc(stored);
    if (tmp == NULL) {
        ESP_LOGE(TAG, "restore: out of memory");
        return;
    }
    if (nvs_get_blob(s_nvs, key, tmp, &stored) == ESP_OK) {
        size_t n = stored - offset;
        if (n > size) {
            n = size;
        }
        memcpy(buffer, tmp + offset, n);
    }
    free(tmp);
}

void smtc_modem_hal_context_store(const modem_context_type_t ctx_type, uint32_t offset, const uint8_t *buffer,
                                  const uint32_t size)
{
    if (!ctx_type_supported(ctx_type)) {
        smtc_modem_hal_on_panic((uint8_t *)__func__, __LINE__, "unsupported context type %d", (int)ctx_type);
        return;
    }
    char key[8];
    ctx_key(ctx_type, key);
    if (nvs_ensure_open() != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }

    size_t stored = 0;
    esp_err_t err = nvs_get_blob(s_nvs, key, NULL, &stored);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        stored = 0;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "store: nvs_get_blob size failed (%s)", esp_err_to_name(err));
        return;
    }

    /* Read-modify-write so a store at an offset leaves the rest of the blob intact. */
    size_t   need = (offset + size > stored) ? (size_t)(offset + size) : stored;
    uint8_t *tmp  = malloc(need);
    if (tmp == NULL) {
        ESP_LOGE(TAG, "store: out of memory");
        return;
    }
    memset(tmp, 0xFF, need);
    if (stored > 0) {
        size_t n = stored;
        if (nvs_get_blob(s_nvs, key, tmp, &n) != ESP_OK) {
            ESP_LOGE(TAG, "store: nvs_get_blob failed");
            free(tmp);
            return;
        }
    }

    /* The stack saves often; skip the flash write when nothing changed. */
    if (offset + size <= stored && memcmp(tmp + offset, buffer, size) == 0) {
        free(tmp);
        return;
    }
    memcpy(tmp + offset, buffer, size);

    err = nvs_set_blob(s_nvs, key, tmp, need);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "store: write failed (%s)", esp_err_to_name(err));
    }
    free(tmp);
}

void smtc_modem_hal_context_flash_pages_erase(const modem_context_type_t ctx_type, uint32_t offset, uint8_t nb_page)
{
    /* Only used by Store-and-Forward, which this port does not provide. */
    (void)offset;
    (void)nb_page;
    smtc_modem_hal_on_panic((uint8_t *)__func__, __LINE__, "flash page erase unsupported (ctx %d)", (int)ctx_type);
}

/* --------------------------------------------------------------- crashlog -- */

void smtc_modem_hal_crashlog_store(const uint8_t *crash_string, uint8_t crash_string_length)
{
    uint8_t n = (crash_string_length > CRASH_LOG_SIZE) ? CRASH_LOG_SIZE : crash_string_length;
    memcpy(s_crash_buf, crash_string, n);
    s_crash_len       = n;
    s_crash_available = 1;
    s_crash_magic     = CRASHLOG_MAGIC;
}

void smtc_modem_hal_crashlog_restore(uint8_t *crash_string, uint8_t *crash_string_length)
{
    if (s_crash_magic != CRASHLOG_MAGIC) {
        *crash_string_length = 0;
        return;
    }
    uint8_t n = (s_crash_len > CRASH_LOG_SIZE) ? CRASH_LOG_SIZE : s_crash_len;
    memcpy(crash_string, s_crash_buf, n);
    *crash_string_length = n;
}

void smtc_modem_hal_crashlog_set_status(bool available)
{
    if (s_crash_magic != CRASHLOG_MAGIC) {
        s_crash_len   = 0;
        s_crash_magic = CRASHLOG_MAGIC;
    }
    s_crash_available = available ? 1 : 0;
}

bool smtc_modem_hal_crashlog_get_status(void)
{
    return (s_crash_magic == CRASHLOG_MAGIC) && (s_crash_available != 0);
}

void smtc_modem_hal_on_panic(uint8_t *func, uint32_t line, const char *fmt, ...)
{
    char out[CRASH_LOG_SIZE];
    int  n = snprintf(out, sizeof(out), "%s:%lu ", (const char *)func, (unsigned long)line);
    if (n < 0) {
        n = 0;
    }
    if ((size_t)n >= sizeof(out)) {
        n = (int)sizeof(out) - 1;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(out + n, sizeof(out) - (size_t)n, fmt, args);
    va_end(args);

    smtc_modem_hal_crashlog_store((const uint8_t *)out, (uint8_t)strnlen(out, sizeof(out)));
    ESP_LOGE(TAG, "Modem panic: %s", out);
    smtc_modem_hal_reset_mcu();
}

/* ----------------------------------------------------------------- random -- */

uint32_t smtc_modem_hal_get_random_nb_in_range(const uint32_t val_1, const uint32_t val_2)
{
    uint32_t lo = (val_1 < val_2) ? val_1 : val_2;
    uint32_t hi = (val_1 < val_2) ? val_2 : val_1;
    uint32_t span = hi - lo;
    if (span == UINT32_MAX) {
        return esp_random();
    }
    return lo + (esp_random() % (span + 1u));
}

/* --------------------------------------------------------------- radio env -- */

bool smtc_modem_external_stack_currently_use_radio(void)
{
    return false; /* the radio is always available to the LBM stack */
}

void smtc_modem_hal_start_radio_tcxo(void)
{
    /* The TCXO, if any, is powered by the radio's DIO3 (see Kconfig); nothing to do here. */
}

void smtc_modem_hal_stop_radio_tcxo(void)
{
}

uint32_t smtc_modem_hal_get_radio_tcxo_startup_delay_ms(void)
{
    return 0; /* radio-controlled TCXO startup is handled by the SX126x itself */
}

void smtc_modem_hal_set_ant_switch(bool is_tx_on)
{
    (void)is_tx_on; /* only SX127x boards use this hook; SX126x uses DIO2 or RXEN/TXEN */
}

/* ------------------------------------------------------------ environment --- */

uint8_t smtc_modem_hal_get_battery_level(void)
{
    /* 0 = external power, 1..254 = level, 255 = unknown. Replace with a real measurement if needed. */
    return 255;
}

int8_t smtc_modem_hal_get_board_delay_ms(void)
{
    return CONFIG_LBM_BOARD_DELAY_MS;
}

int8_t smtc_modem_hal_get_temperature(void)
{
    return 25; /* placeholder, used only by cloud services */
}

uint16_t smtc_modem_hal_get_voltage_mv(void)
{
    return 3300; /* placeholder, used only by cloud services */
}

/* ------------------------------------------------------------------ trace --- */

void smtc_modem_hal_print_trace(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

/* -------------------------- FUOTA / Store-and-Forward (not provided here) --- */

uint32_t smtc_modem_hal_get_hw_version_for_fuota(void)
{
    return 0;
}

uint32_t smtc_modem_hal_get_fw_version_for_fuota(void)
{
    return 0;
}

uint8_t smtc_modem_hal_get_fw_status_available_for_fuota(void)
{
    return 0;
}

uint8_t smtc_modem_hal_get_fw_delete_status_for_fuota(uint32_t fw_to_delete_version)
{
    (void)fw_to_delete_version;
    return 0;
}

uint32_t smtc_modem_hal_get_next_fw_version_for_fuota(void)
{
    return 0;
}

uint16_t smtc_modem_hal_store_and_forward_get_number_of_pages(void)
{
    return 0;
}

uint16_t smtc_modem_hal_flash_get_page_size(void)
{
    return 0;
}
