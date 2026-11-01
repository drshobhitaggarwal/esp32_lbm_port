/*
 * ESP32 runtime for LoRa Basics Modem: board bring-up, modem task, and the
 * interrupt dispatch that replaces the bare-metal timer/DIO1 ISRs.
 *
 * Why a dispatch task instead of calling the stack from the ISRs
 *   The stack expects its timer and radio callbacks to run in interrupt context
 *   and to be atomic with respect to the main loop (smtc_modem_hal_disable/
 *   enable_modem_irq bracket its critical sections). The radio callback also
 *   timestamps the IRQ, and that timestamp schedules RX1/RX2, so it must not be
 *   delayed behind a long engine run.
 *
 *   Here the real ISRs (IRAM, no flash access) only record a pending bit and
 *   notify a task. That task runs at a priority above the modem task, pinned to
 *   the same core, so it preempts the engine immediately and, being on the same
 *   core, cannot run while the engine is inside a masked section. Masking is a
 *   flag the dispatch task honours; pending events replay on unmask.
 */

#include <string.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lbm_esp32.h"
#include "lbm_esp32_priv.h"
#include "smtc_modem_api.h"
#include "smtc_modem_utilities.h"

static const char *TAG = "lbm";

#define LBM_IRQ_TIMER (1u << 0)
#define LBM_IRQ_RADIO (1u << 1)

#define LBM_IRQ_TASK_PRIORITY (configMAX_PRIORITIES - 2)
#define LBM_MAX_SLEEP_MS      30000u

lbm_esp32_config_t g_lbm_cfg;
spi_device_handle_t g_lbm_spi;

typedef struct {
    void (*cb)(void *ctx);
    void *ctx;
} lbm_cb_t;

static TaskHandle_t      s_modem_task;
static TaskHandle_t      s_irq_task;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_timer;
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile unsigned s_pending; /* LBM_IRQ_* bits, guarded by s_mux */
static volatile bool     s_irq_masked;
static lbm_cb_t          s_timer_cb;
static lbm_cb_t          s_radio_cb;
static bool              s_started;

/* ------------------------------------------------------------------ ISRs -- */

/* Pending-event bits are shared between ISRs and tasks. A spinlock keeps this IRAM-safe on every
 * chip (C11 atomics may compile to flash-resident library calls where the core lacks atomics). */
static void IRAM_ATTR pending_set_from_isr(unsigned bit)
{
    portENTER_CRITICAL_ISR(&s_mux);
    s_pending |= bit;
    portEXIT_CRITICAL_ISR(&s_mux);
}

#if !CONFIG_ESP_TIMER_SUPPORT_ISR_DISPATCH_METHOD
static void pending_set_from_task(unsigned bit)
{
    portENTER_CRITICAL(&s_mux);
    s_pending |= bit;
    portEXIT_CRITICAL(&s_mux);
}
#endif

/* Clear `mask` and return the bits that were set within it. */
static unsigned pending_take(unsigned mask)
{
    portENTER_CRITICAL(&s_mux);
    unsigned v = s_pending & mask;
    s_pending &= ~mask;
    portEXIT_CRITICAL(&s_mux);
    return v;
}


static void IRAM_ATTR dio1_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    pending_set_from_isr(LBM_IRQ_RADIO);
    vTaskNotifyGiveFromISR(s_irq_task, &woken);
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

#if CONFIG_ESP_TIMER_SUPPORT_ISR_DISPATCH_METHOD
static void IRAM_ATTR timer_cb(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    pending_set_from_isr(LBM_IRQ_TIMER);
    vTaskNotifyGiveFromISR(s_irq_task, &woken);
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}
#else
static void timer_cb(void *arg)
{
    (void)arg;
    pending_set_from_task(LBM_IRQ_TIMER);
    xTaskNotifyGive(s_irq_task);
}
#endif

/* --------------------------------------------------- IRQ dispatch task ---- */

static void irq_dispatch_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (!s_irq_masked) {
            unsigned bits = pending_take(LBM_IRQ_TIMER | LBM_IRQ_RADIO);
            if (bits == 0) {
                break;
            }
            if ((bits & LBM_IRQ_TIMER) && s_timer_cb.cb) {
                s_timer_cb.cb(s_timer_cb.ctx);
            }
            if ((bits & LBM_IRQ_RADIO) && s_radio_cb.cb) {
                s_radio_cb.cb(s_radio_cb.ctx);
            }
        }
    }
}

/* ------------------------------------------- hooks used by the modem HAL -- */

void lbm_esp32_set_radio_irq_cb(void (*cb)(void *ctx), void *ctx)
{
    s_radio_cb.ctx = ctx;
    s_radio_cb.cb  = cb;
}

void lbm_esp32_timer_start(uint32_t ms, void (*cb)(void *ctx), void *ctx)
{
    esp_timer_stop(s_timer); /* returns an error if not running; that is fine */
    pending_take(LBM_IRQ_TIMER);
    s_timer_cb.ctx = ctx;
    s_timer_cb.cb  = cb;
    uint64_t us = (uint64_t)ms * 1000ULL;
    if (us == 0) {
        us = 1;
    }
    ESP_ERROR_CHECK(esp_timer_start_once(s_timer, us));
}

void lbm_esp32_timer_stop(void)
{
    esp_timer_stop(s_timer);
    pending_take(LBM_IRQ_TIMER);
}

void lbm_esp32_irq_mask(bool masked)
{
    s_irq_masked = masked;
    if (!masked && s_irq_task) {
        xTaskNotifyGive(s_irq_task); /* replay anything that arrived while masked */
    }
}

void lbm_esp32_notify_modem_task(void)
{
    if (s_modem_task) {
        xTaskNotifyGive(s_modem_task);
    }
}

/* ------------------------------------------------------------- board init -- */

lbm_esp32_config_t lbm_esp32_default_config(void)
{
    lbm_esp32_config_t c = {
#if defined(CONFIG_LBM_SPI_HOST_SPI3)
        .spi_host = SPI3_HOST,
#else
        .spi_host = SPI2_HOST,
#endif
        .pin_mosi         = CONFIG_LBM_PIN_MOSI,
        .pin_miso         = CONFIG_LBM_PIN_MISO,
        .pin_sclk         = CONFIG_LBM_PIN_SCLK,
        .pin_nss          = CONFIG_LBM_PIN_NSS,
        .pin_nrst         = CONFIG_LBM_PIN_NRST,
        .pin_busy         = CONFIG_LBM_PIN_BUSY,
        .pin_dio1         = CONFIG_LBM_PIN_DIO1,
        .pin_rxen         = CONFIG_LBM_PIN_RXEN,
        .pin_txen         = CONFIG_LBM_PIN_TXEN,
        .fixed_pin        = { CONFIG_LBM_PIN_FIXED1, CONFIG_LBM_PIN_FIXED2, CONFIG_LBM_PIN_FIXED3,
                              CONFIG_LBM_PIN_FIXED4 },
        .fixed_level      = { CONFIG_LBM_LEVEL_FIXED1, CONFIG_LBM_LEVEL_FIXED2, CONFIG_LBM_LEVEL_FIXED3,
                              CONFIG_LBM_LEVEL_FIXED4 },
        .spi_clock_hz     = CONFIG_LBM_SPI_CLOCK_HZ,
#if CONFIG_FREERTOS_UNICORE
        .task_core        = 0,
#else
        .task_core        = CONFIG_LBM_TASK_CORE,
#endif
        .task_priority    = CONFIG_LBM_TASK_PRIORITY,
        .task_stack_bytes = CONFIG_LBM_TASK_STACK_BYTES,
    };
    return c;
}

static esp_err_t board_init(const lbm_esp32_config_t *c)
{
    esp_err_t err;

    /* Outputs: NSS idles high, NRESET idles high (released). */
    gpio_set_level(c->pin_nss, 1);
    gpio_set_level(c->pin_nrst, 1);
    uint64_t out_mask = (1ULL << c->pin_nss) | (1ULL << c->pin_nrst);
    if (c->pin_rxen >= 0) {
        out_mask |= 1ULL << c->pin_rxen;
    }
    if (c->pin_txen >= 0) {
        out_mask |= 1ULL << c->pin_txen;
    }
    gpio_config_t out = {
        .pin_bit_mask = out_mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&out);
    if (err != ESP_OK) {
        return err;
    }
    if (c->pin_rxen >= 0) {
        gpio_set_level(c->pin_rxen, 0);
    }
    if (c->pin_txen >= 0) {
        gpio_set_level(c->pin_txen, 0);
    }

    /* Front-end power/enable lines: set the level first so the pin never glitches. */
    for (int i = 0; i < 4; i++) {
        if (c->fixed_pin[i] < 0) {
            continue;
        }
        gpio_set_level(c->fixed_pin[i], c->fixed_level[i]);
        gpio_config_t fx = {
            .pin_bit_mask = 1ULL << c->fixed_pin[i],
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&fx);
        if (err != ESP_OK) {
            return err;
        }
        gpio_set_level(c->fixed_pin[i], c->fixed_level[i]);
    }

    gpio_config_t busy = {
        .pin_bit_mask = 1ULL << c->pin_busy,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&busy);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t dio1 = {
        .pin_bit_mask = 1ULL << c->pin_dio1,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    err = gpio_config(&dio1);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* already installed */
        return err;
    }
    err = gpio_isr_handler_add(c->pin_dio1, dio1_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }

    spi_bus_config_t bus = {
        .mosi_io_num     = c->pin_mosi,
        .miso_io_num     = c->pin_miso,
        .sclk_io_num     = c->pin_sclk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 300,
    };
    err = spi_bus_initialize((spi_host_device_t)c->spi_host, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* bus may be shared and already up */
        return err;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = (int)c->spi_clock_hz,
        .mode           = 0,
        .spics_io_num   = -1, /* NSS is driven by hand: wake-up needs it held low while polling BUSY */
        .queue_size     = 1,
    };
    return spi_bus_add_device((spi_host_device_t)c->spi_host, &dev, &g_lbm_spi);
}

/* ------------------------------------------------------------- modem task -- */

static void modem_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
        uint32_t sleep_ms = smtc_modem_run_engine();
        bool     pending  = smtc_modem_is_irq_flag_pending();
        xSemaphoreGiveRecursive(s_lock);

        if (!pending) {
            if (sleep_ms > LBM_MAX_SLEEP_MS) {
                sleep_ms = LBM_MAX_SLEEP_MS;
            }
            /* Timer and radio events wake this task by notification, so this
             * is an upper bound only. A notification that arrived while the
             * engine ran is still pending and ends the wait at once. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(sleep_ms));
        }
    }
}

/* ------------------------------------------------------------- public API -- */

esp_err_t lbm_esp32_start(const lbm_esp32_config_t *cfg, void (*event_cb)(void))
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (event_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    g_lbm_cfg = cfg ? *cfg : lbm_esp32_default_config();
    const lbm_esp32_config_t *c = &g_lbm_cfg;

    if (c->pin_mosi < 0 || c->pin_miso < 0 || c->pin_sclk < 0 || c->pin_nss < 0 ||
        c->pin_nrst < 0 || c->pin_busy < 0 || c->pin_dio1 < 0) {
        ESP_LOGE(TAG, "radio pins are not configured");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_flash_init(); /* ESP_OK when the app already did it */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed (%s); initialise NVS before lbm_esp32_start()",
                 esp_err_to_name(err));
        return err;
    }

    s_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(irq_dispatch_task, "lbm_irq", 3072, NULL, LBM_IRQ_TASK_PRIORITY,
                                &s_irq_task, c->task_core) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    esp_timer_create_args_t targs = {
        .callback = timer_cb,
        .arg      = NULL,
        .name     = "lbm",
#if CONFIG_ESP_TIMER_SUPPORT_ISR_DISPATCH_METHOD
        .dispatch_method = ESP_TIMER_ISR,
#else
        .dispatch_method = ESP_TIMER_TASK,
#endif
    };
    err = esp_timer_create(&targs, &s_timer);
    if (err != ESP_OK) {
        return err;
    }

    err = board_init(c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    smtc_modem_init(event_cb); /* resets and configures the radio through the HAL */
#if CONFIG_LBM_CRYSTAL_ERROR_PPM > 0
    smtc_modem_return_code_t rc = smtc_modem_set_crystal_error_ppm(CONFIG_LBM_CRYSTAL_ERROR_PPM);
    if (rc != SMTC_MODEM_RC_OK) {
        ESP_LOGW(TAG, "set_crystal_error_ppm returned %d", (int)rc);
    }
#endif
    xSemaphoreGiveRecursive(s_lock);

    if (xTaskCreatePinnedToCore(modem_task, "lbm", c->task_stack_bytes, NULL, c->task_priority,
                                &s_modem_task, c->task_core) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "started on core %d", c->task_core);
    return ESP_OK;
}

void lbm_esp32_lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void lbm_esp32_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

void lbm_esp32_wake(void)
{
    lbm_esp32_notify_modem_task();
}
