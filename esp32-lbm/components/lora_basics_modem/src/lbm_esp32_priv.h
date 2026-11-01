/* Internal declarations shared between the port's source files. */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "lbm_esp32.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set by lbm_esp32_start(), read by the radio HAL. */
extern lbm_esp32_config_t g_lbm_cfg;
extern spi_device_handle_t g_lbm_spi;

/* Callback registration used by smtc_modem_hal_irq_config_radio_irq(). */
void lbm_esp32_set_radio_irq_cb(void (*cb)(void *ctx), void *ctx);

/* One-shot timer backing smtc_modem_hal_start_timer() / stop_timer(). */
void lbm_esp32_timer_start(uint32_t ms, void (*cb)(void *ctx), void *ctx);
void lbm_esp32_timer_stop(void);

/* Mask / unmask delivery of the timer and radio callbacks. */
void lbm_esp32_irq_mask(bool masked);

/* Wake the modem task from the IRQ dispatch task (smtc_modem_hal_user_lbm_irq). */
void lbm_esp32_notify_modem_task(void);

#ifdef __cplusplus
}
#endif
