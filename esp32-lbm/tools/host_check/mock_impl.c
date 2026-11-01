/* Host-side stand-ins for the ESP-IDF calls the port uses. Only what is needed to link and to
 * exercise the port's own logic (context storage, SPI framing, BUSY/wake handling). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_rom_sys.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "mock_state.h"

mock_state_t g_mock;

const char *esp_err_to_name(esp_err_t c) { (void)c; return "err"; }

/* ---- timers / time ---- */
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *o)
{
    g_mock.timer_dispatch = a->dispatch_method; g_mock.timer_cb = a->callback;
    *o = (esp_timer_handle_t)&g_mock;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) { (void)t; g_mock.timer_armed_us = us; return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t t) { (void)t; g_mock.timer_armed_us = 0; return ESP_OK; }
int64_t esp_timer_get_time(void)
{
    if (g_mock.fake_time_us >= 0) return g_mock.fake_time_us;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
uint32_t esp_random(void)
{
    static uint32_t x = 2463534242u;
    if (g_mock.random_value) return g_mock.random_value; /* pinned by a test */
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}
void esp_restart(void) { g_mock.restart_called++; }
esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
void esp_rom_delay_us(uint32_t us) { g_mock.delay_us_total += us; }

/* ---- NVS: in-memory blobs ---- */
#define MAX_KEYS 8
static struct { char key[16]; uint8_t *data; size_t len; } s_nvs[MAX_KEYS];

esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    for (int i = 0; i < MAX_KEYS; i++) {
        if (s_nvs[i].data && !strcmp(s_nvs[i].key, key)) {
            if (out == NULL) { *len = s_nvs[i].len; return ESP_OK; }
            if (*len < s_nvs[i].len) return ESP_FAIL;
            memcpy(out, s_nvs[i].data, s_nvs[i].len); *len = s_nvs[i].len; return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len)
{
    (void)h;
    int slot = -1;
    for (int i = 0; i < MAX_KEYS; i++) {
        if (s_nvs[i].data && !strcmp(s_nvs[i].key, key)) { slot = i; break; }
        if (!s_nvs[i].data && slot < 0) slot = i;
    }
    if (slot < 0) return ESP_FAIL;
    free(s_nvs[slot].data);
    s_nvs[slot].data = malloc(len); memcpy(s_nvs[slot].data, v, len);
    s_nvs[slot].len = len; snprintf(s_nvs[slot].key, sizeof s_nvs[slot].key, "%s", key);
    g_mock.nvs_set_calls++;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; g_mock.nvs_commit_calls++; return ESP_OK; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }

/* ---- GPIO ---- */
esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t p, uint32_t l)
{
    if (p >= 0 && p < 64) { g_mock.gpio[p] = (int)l; g_mock.gpio_events[g_mock.gpio_event_n % 64] = p * 10 + (int)l; g_mock.gpio_event_n++; }
    return ESP_OK;
}
int gpio_get_level(gpio_num_t p)
{
    if (p == g_mock.busy_pin) {
        if (g_mock.busy_stuck) return 1;
        /* BUSY reads high while NSS is low in sleep-wake, then drops after N polls */
        if (g_mock.busy_polls_remaining > 0) { g_mock.busy_polls_remaining--; return 1; }
        return 0;
    }
    return g_mock.gpio[p];
}
esp_err_t gpio_install_isr_service(int f) { (void)f; return g_mock.isr_service_installed++ ? ESP_ERR_INVALID_STATE : ESP_OK; }
esp_err_t gpio_isr_handler_add(gpio_num_t p, gpio_isr_t i, void *a) { (void)p; (void)a; g_mock.dio1_isr = i; return ESP_OK; }

/* ---- SPI: record frames, answer with a known pattern ---- */
esp_err_t spi_bus_initialize(spi_host_device_t h, const spi_bus_config_t *c, int d) { (void)h; (void)c; (void)d; return ESP_OK; }
esp_err_t spi_bus_add_device(spi_host_device_t h, const spi_device_interface_config_t *c, spi_device_handle_t *o)
{ (void)h; (void)c; *o = (spi_device_handle_t)&g_mock; return ESP_OK; }
esp_err_t spi_device_polling_transmit(spi_device_handle_t h, spi_transaction_t *t)
{
    (void)h;
    size_t n = t->length / 8;
    if (g_mock.spi_frames < 16) {
        memcpy(g_mock.spi_tx[g_mock.spi_frames], t->tx_buffer, n > 300 ? 300 : n);
        g_mock.spi_len[g_mock.spi_frames] = n;
        g_mock.spi_nss_at_xfer[g_mock.spi_frames] = g_mock.gpio[g_mock.nss_pin];
    }
    g_mock.spi_frames++;
    if (t->rx_buffer) for (size_t i = 0; i < n; i++) ((uint8_t *)t->rx_buffer)[i] = g_mock.spi_rx_zero ? 0 : (uint8_t)(0xA0 + i);
    return g_mock.spi_fail ? ESP_FAIL : ESP_OK;
}

/* ---- FreeRTOS: no scheduler on the host ---- */
void host_yield_from_isr(void) {}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t f, const char *n, uint32_t s, void *a, UBaseType_t p, TaskHandle_t *o, BaseType_t c)
{ (void)s; (void)a; (void)p; (void)c; if (!strcmp(n, "lbm_irq")) g_mock.irq_task = f; else g_mock.modem_task = f; if (o) *o = (TaskHandle_t)&g_mock; return pdPASS; }
void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *w) { (void)t; if (w) *w = pdFALSE; }
BaseType_t xTaskNotifyGive(TaskHandle_t t) { (void)t; return pdPASS; }
#include <setjmp.h>
uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t t)
{
    (void)c; (void)t;
    /* Let a task body loop run N iterations, then jump back to the test. */
    if (g_mock.exit_jmp && g_mock.take_calls_before_exit-- <= 0) longjmp(*(jmp_buf *)g_mock.exit_jmp, 1);
    return 0;
}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return (SemaphoreHandle_t)&g_mock; }
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
