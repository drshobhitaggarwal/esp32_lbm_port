#pragma once
#include <stdint.h>
typedef struct {
    int timer_dispatch; uint64_t timer_armed_us; int64_t fake_time_us;
    uint32_t random_value; int restart_called; uint64_t delay_us_total;
    int nvs_set_calls, nvs_commit_calls;
    int gpio[64]; int gpio_events[64]; int gpio_event_n;
    int spi_rx_zero; void (*dio1_isr)(void *); void (*timer_cb)(void *); void (*irq_task)(void *); void (*modem_task)(void *);
    int take_calls_before_exit; void *exit_jmp;
    int busy_pin, nss_pin, busy_stuck, busy_polls_remaining, isr_service_installed;
    uint8_t spi_tx[16][300]; int spi_len[16]; int spi_nss_at_xfer[16]; int spi_frames; int spi_fail;
} mock_state_t;
extern mock_state_t g_mock;
