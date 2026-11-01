/* Host-side checks for the ESP32 port. Exercises the port's own logic against mocked ESP-IDF calls:
 * context storage, SPI framing, BUSY/wake, RF switch pins, time wrap, random range, crash log,
 * and the timer/radio IRQ dispatch (mask, replay, stop). Then starts the real LBM stack on a
 * fake radio and runs the engine once. It cannot prove behaviour on silicon. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "lbm_esp32.h"
#include "lbm_esp32_priv.h"
#include "smtc_modem_api.h"
#include "smtc_modem_hal.h"
#include "smtc_modem_utilities.h"
#include "sx126x_hal.h"
#include "ral_sx126x_bsp.h"
#include "mock_state.h"

static int s_fail;
#define CHECK(cond, msg) do { if (cond) printf("  ok   %s\n", msg); else { printf("  FAIL %s  (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } } while (0)

static void run_task(void (*fn)(void *), int iterations)
{
    jmp_buf jb;
    g_mock.exit_jmp = &jb;
    g_mock.take_calls_before_exit = iterations;
    if (setjmp(jb) == 0) fn(NULL);
    g_mock.exit_jmp = NULL;
}

/* ------------------------------------------------------------- radio HAL -- */
static void test_radio_hal(void)
{
    printf("radio HAL\n");
    g_lbm_cfg = lbm_esp32_default_config();
    g_lbm_spi = (spi_device_handle_t)&g_mock;
    g_mock.busy_pin = g_lbm_cfg.pin_busy;
    g_mock.nss_pin  = g_lbm_cfg.pin_nss;

    const uint8_t cmd[] = { 0x0D, 0x09, 0x20 };
    const uint8_t dat[] = { 1, 2, 3 };
    g_mock.spi_frames = 0;
    CHECK(sx126x_hal_write(NULL, cmd, 3, dat, 3) == SX126X_HAL_STATUS_OK, "write returns OK");
    CHECK(g_mock.spi_frames == 1 && g_mock.spi_len[0] == 6, "write is one 6-byte frame");
    CHECK(!memcmp(g_mock.spi_tx[0], "\x0D\x09\x20\x01\x02\x03", 6), "write sends command then data");
    CHECK(g_mock.spi_nss_at_xfer[0] == 0 && g_mock.gpio[g_lbm_cfg.pin_nss] == 1, "NSS low during frame, high after");

    uint8_t out[2] = { 0, 0 };
    const uint8_t rcmd[] = { 0x12, 0x00, 0x00, 0x00 };
    g_mock.spi_frames = 0;
    CHECK(sx126x_hal_read(NULL, rcmd, 4, out, 2) == SX126X_HAL_STATUS_OK, "read returns OK");
    CHECK(g_mock.spi_len[0] == 6, "read is one frame of command+data length");
    CHECK(out[0] == 0xA4 && out[1] == 0xA5, "read returns the bytes clocked after the command");
    CHECK(!memcmp(g_mock.spi_tx[0], "\x12\0\0\0\0\0", 6), "read clocks zeros for the data phase");

    /* SetSleep then another command: the wake-up must hold NSS low while BUSY is polled. */
    const uint8_t sleep_cmd[] = { 0x84, 0x00 };
    CHECK(sx126x_hal_write(NULL, sleep_cmd, 2, NULL, 0) == SX126X_HAL_STATUS_OK, "SetSleep accepted");
    g_mock.gpio_event_n = 0;
    g_mock.busy_polls_remaining = 5;
    CHECK(sx126x_hal_write(NULL, cmd, 3, NULL, 0) == SX126X_HAL_STATUS_OK, "write after sleep OK");
    int n = g_mock.gpio_event_n;
    int nss = g_lbm_cfg.pin_nss;
    CHECK(n >= 4 && g_mock.gpio_events[0] == nss * 10 + 0 && g_mock.gpio_events[1] == nss * 10 + 1,
          "wake: NSS low, then high before the frame");

    g_mock.busy_stuck = 1;
    CHECK(sx126x_hal_write(NULL, cmd, 3, NULL, 0) == SX126X_HAL_STATUS_ERROR, "stuck BUSY returns ERROR (not a hang)");
    g_mock.busy_stuck = 0;

    g_mock.spi_fail = 1;
    CHECK(sx126x_hal_write(NULL, cmd, 3, NULL, 0) == SX126X_HAL_STATUS_ERROR, "SPI failure returns ERROR");
    g_mock.spi_fail = 0;

    uint8_t big[300];
    CHECK(sx126x_hal_write(NULL, cmd, 3, big, 300) == SX126X_HAL_STATUS_ERROR, "oversize frame rejected");

#if CONFIG_LBM_PIN_TXEN >= 0
    {
        const uint8_t tx_op[] = { 0x83, 0, 0, 0 }, rx_op[] = { 0x82, 0, 0, 0 }, sb_op[] = { 0x80, 0 };
        const int txen = CONFIG_LBM_PIN_TXEN;
#if CONFIG_LBM_PIN_RXEN >= 0
        const int rxen = CONFIG_LBM_PIN_RXEN;
#define RXEN_LEVEL(p) g_mock.gpio[p]
#else
        const int rxen = -1;
#define RXEN_LEVEL(p) 0
#endif
        sx126x_hal_write(NULL, tx_op, 4, NULL, 0);
        CHECK(g_mock.gpio[txen] == 1 && RXEN_LEVEL(rxen) == 0, "SetTx -> TXEN high (RXEN low if present)");
        sx126x_hal_write(NULL, rx_op, 4, NULL, 0);
        CHECK(g_mock.gpio[txen] == 0 && (rxen < 0 || g_mock.gpio[rxen] == 1), "SetRx -> TXEN low (RXEN high if present)");
        sx126x_hal_write(NULL, tx_op, 4, NULL, 0);
        sx126x_hal_write(NULL, sb_op, 2, NULL, 0);
        CHECK(g_mock.gpio[txen] == 0 && RXEN_LEVEL(rxen) == 0, "SetStandby -> both low");
    }
#endif
}


/* ------------------------------------------------------------------ BSP --- */
static void test_bsp(void)
{
    printf("radio BSP (Kconfig-driven)\n");
    ral_xosc_cfg_t xosc; sx126x_tcxo_ctrl_voltages_t volt = SX126X_TCXO_CTRL_3_3V; uint32_t ticks = 0;
    ral_sx126x_bsp_get_xosc_cfg(NULL, &xosc, &volt, &ticks);
#if defined(CONFIG_LBM_BOARD_HELTEC)
    CHECK(xosc == RAL_XOSC_CFG_TCXO_RADIO_CTRL, "Heltec preset: TCXO controlled by the radio");
    CHECK(volt == SX126X_TCXO_CTRL_1_8V, "Heltec preset: 1.8 V TCXO supply");
    CHECK(ticks == 5 * 64, "Heltec preset: 5 ms start-up = 320 ticks of 15.625 us");
    bool sw = false; ral_sx126x_bsp_get_rf_switch_cfg(NULL, &sw);
    CHECK(sw, "Heltec preset: DIO2 drives the RF switch");
    sx126x_reg_mod_t reg; ral_sx126x_bsp_get_reg_mode(NULL, &reg);
    CHECK(reg == SX126X_REG_MODE_DCDC, "Heltec preset: DC-DC regulator");
    CHECK(g_lbm_cfg.pin_nss == 8 && g_lbm_cfg.pin_sclk == 9 && g_lbm_cfg.pin_mosi == 10 && g_lbm_cfg.pin_miso == 11 &&
          g_lbm_cfg.pin_nrst == 12 && g_lbm_cfg.pin_busy == 13 && g_lbm_cfg.pin_dio1 == 14,
          "Heltec preset: NSS 8, SCK 9, MOSI 10, MISO 11, RST 12, BUSY 13, DIO1 14");
#endif
#if defined(CONFIG_LBM_RADIO_SX1262) || defined(CONFIG_LBM_RADIO_SX1268)
    ral_sx126x_bsp_tx_cfg_input_params_t in = { .system_output_pwr_in_dbm = 14, .freq_in_hz = 868100000 };
    ral_sx126x_bsp_tx_cfg_output_params_t out; memset(&out, 0, sizeof out);
    ral_sx126x_bsp_get_tx_cfg(NULL, &in, &out);
    CHECK(out.pa_cfg.device_sel == 0 && out.pa_cfg.hp_max == 7 &&
          out.chip_output_pwr_in_dbm_configured == 14 + CONFIG_LBM_TX_POWER_OFFSET_DB,
          "SX1262 PA: high-power PA, requested power plus board offset");
    in.system_output_pwr_in_dbm = 40;
    ral_sx126x_bsp_get_tx_cfg(NULL, &in, &out);
    CHECK(out.chip_output_pwr_in_dbm_configured == 22, "SX1262 PA: power clamped to +22 dBm");
#endif
}

/* ------------------------------------------------------------ modem HAL --- */
static void test_modem_hal(void)
{
    printf("modem HAL\n");
    uint8_t buf[8];

    memset(buf, 0, sizeof buf);
    smtc_modem_hal_context_restore(CONTEXT_MODEM, 0, buf, 8);
    CHECK(buf[0] == 0xFF && buf[7] == 0xFF, "missing context reads back as 0xFF");

    const uint8_t a[4] = { 1, 2, 3, 4 };
    int before = g_mock.nvs_set_calls;
    smtc_modem_hal_context_store(CONTEXT_MODEM, 0, a, 4);
    CHECK(g_mock.nvs_set_calls == before + 1, "first store writes NVS");
    memset(buf, 0, sizeof buf);
    smtc_modem_hal_context_restore(CONTEXT_MODEM, 0, buf, 4);
    CHECK(!memcmp(buf, a, 4), "restore returns what was stored");

    smtc_modem_hal_context_store(CONTEXT_MODEM, 0, a, 4);
    CHECK(g_mock.nvs_set_calls == before + 1, "identical store skips the flash write");

    const uint8_t b[4] = { 9, 8, 7, 6 };
    smtc_modem_hal_context_store(CONTEXT_MODEM, 4, b, 4);
    smtc_modem_hal_context_restore(CONTEXT_MODEM, 0, buf, 8);
    CHECK(!memcmp(buf, a, 4) && !memcmp(buf + 4, b, 4), "store at an offset keeps the rest of the blob");

    uint8_t other[4];
    smtc_modem_hal_context_restore(CONTEXT_KEY_MODEM, 0, other, 4);
    CHECK(other[0] == 0xFF, "context types are independent");

    smtc_modem_hal_context_restore(CONTEXT_MODEM, 6, buf, 8);
    CHECK(buf[0] == 7 && buf[1] == 6 && buf[2] == 0xFF, "restore past stored length pads with 0xFF");

    /* time */
    g_mock.fake_time_us = ((int64_t)0xFFFFFFFFLL + 2) * 1000;
    CHECK(smtc_modem_hal_get_time_in_ms() == 1, "ms counter wraps 0xFFFFFFFF -> 0");
    g_mock.fake_time_us = 5 * 1000000LL + 400;
    CHECK(smtc_modem_hal_get_time_in_s() == 5 && smtc_modem_hal_get_time_in_ms() == 5000, "s and ms agree");
    g_mock.fake_time_us = -1;

    /* random */
    g_mock.random_value = 1000;
    CHECK(smtc_modem_hal_get_random_nb_in_range(10, 20) == 10 + 1000 % 11, "random in [min,max]");
    CHECK(smtc_modem_hal_get_random_nb_in_range(20, 10) == 10 + 1000 % 11, "random accepts reversed bounds");
    CHECK(smtc_modem_hal_get_random_nb_in_range(0, 0xFFFFFFFFu) == 1000, "random full range does not overflow");
    CHECK(smtc_modem_hal_get_random_nb_in_range(7, 7) == 7, "random with equal bounds");
    g_mock.random_value = 0; /* back to a PRNG: the stack retries until it draws different values */

    /* crash log */
    uint8_t out[CRASH_LOG_SIZE], len = 0;
    CHECK(!smtc_modem_hal_crashlog_get_status(), "no crash log before a panic");
    int r = g_mock.restart_called;
    smtc_modem_hal_on_panic((uint8_t *)"myfunc", 42, "bad value %d", 7);
    CHECK(g_mock.restart_called == r + 1, "panic restarts the MCU");
    CHECK(smtc_modem_hal_crashlog_get_status(), "crash log available after panic");
    smtc_modem_hal_crashlog_restore(out, &len);
    CHECK(len > 0 && !strncmp((char *)out, "myfunc:42 bad value 7", len), "crash log holds function, line and message");
    smtc_modem_hal_crashlog_set_status(false);
    CHECK(!smtc_modem_hal_crashlog_get_status(), "crash log status can be cleared");
    char huge[400]; memset(huge, 'x', 399); huge[399] = 0;
    smtc_modem_hal_on_panic((uint8_t *)"f", 1, "%s", huge);
    smtc_modem_hal_crashlog_restore(out, &len);
    CHECK(len <= CRASH_LOG_SIZE, "oversized panic message is truncated to the log size");

    CHECK(smtc_modem_hal_get_board_delay_ms() == CONFIG_LBM_BOARD_DELAY_MS, "board delay comes from Kconfig");
}

/* --------------------------------------------- stack start + IRQ dispatch -- */
static int s_events;
static int s_first_event = -1;
static void event_cb(void)
{
    smtc_modem_event_t ev; uint8_t pending = 0;
    do {
        if (smtc_modem_get_event(&ev, &pending) != SMTC_MODEM_RC_OK) return;
        if (s_first_event < 0) s_first_event = (int)ev.event_type;
        s_events++;
    } while (pending > 0);
}

static int s_timer_hits, s_timer_ctx_seen;
static void timer_user_cb(void *ctx) { s_timer_hits++; s_timer_ctx_seen = (ctx == &s_timer_hits); }

static void test_stack_and_dispatch(void)
{
    printf("stack start on a fake radio\n");
    g_mock.spi_rx_zero = 1;
    esp_err_t err = lbm_esp32_start(NULL, event_cb);
    CHECK(err == ESP_OK, "lbm_esp32_start returns ESP_OK");
    for (int i = 0; i < 4; i++) {
        if (g_lbm_cfg.fixed_pin[i] >= 0) {
            char m[80]; snprintf(m, sizeof m, "fixed GPIO %d held at level %d", g_lbm_cfg.fixed_pin[i], g_lbm_cfg.fixed_level[i]);
            CHECK(g_mock.gpio[g_lbm_cfg.fixed_pin[i]] == g_lbm_cfg.fixed_level[i], m);
        }
    }
#if defined(CONFIG_LBM_BOARD_HELTEC_V4_2)
    CHECK(g_lbm_cfg.fixed_pin[0] == 7 && g_lbm_cfg.fixed_pin[1] == 2 && g_lbm_cfg.fixed_pin[2] == 46 &&
          g_lbm_cfg.fixed_pin[3] == 36 && g_lbm_cfg.fixed_level[3] == 0 && g_lbm_cfg.pin_txen == -1,
          "V4.2 preset: GPIO7/2/46 high, Vext (36) low, no host TXEN");
#elif defined(CONFIG_LBM_BOARD_HELTEC_V4_3)
    CHECK(g_lbm_cfg.fixed_pin[0] == 7 && g_lbm_cfg.fixed_pin[1] == 2 && g_lbm_cfg.fixed_pin[2] == -1 &&
          g_lbm_cfg.fixed_pin[3] == 36 && g_lbm_cfg.pin_txen == 5,
          "V4.3 preset: GPIO7/2 high, Vext (36) low, CTX (5) as TXEN");
#elif defined(CONFIG_LBM_BOARD_HELTEC_V3)
    CHECK(g_lbm_cfg.fixed_pin[0] == -1 && g_lbm_cfg.pin_txen == -1 && g_lbm_cfg.pin_rxen == -1,
          "V3 preset: no external front-end pins");
#endif
    int restarts = g_mock.restart_called;
    CHECK(g_mock.irq_task && g_mock.modem_task && g_mock.dio1_isr && g_mock.timer_cb, "tasks, DIO1 ISR and timer registered");
    CHECK(lbm_esp32_start(NULL, event_cb) == ESP_ERR_INVALID_STATE, "second start is rejected");

    run_task(g_mock.modem_task, 0);
    CHECK(g_mock.restart_called == restarts, "engine ran without a stack panic");
    CHECK(s_first_event == SMTC_MODEM_EVENT_RESET, "first event is RESET");

    printf("IRQ dispatch\n");
    /* radio IRQ while masked: held, then replayed on unmask */
    CHECK(!smtc_modem_is_irq_flag_pending(), "no IRQ flag pending yet");
    smtc_modem_hal_disable_modem_irq();
    g_mock.dio1_isr(NULL);
    run_task(g_mock.irq_task, 1);
    CHECK(!smtc_modem_is_irq_flag_pending(), "masked: radio callback not delivered");
    smtc_modem_hal_enable_modem_irq();
    run_task(g_mock.irq_task, 1);
    CHECK(smtc_modem_is_irq_flag_pending(), "unmasked: radio callback replayed");

    /* timer: fires once, with its context; stop cancels a pending fire */
    smtc_modem_hal_start_timer(10, timer_user_cb, &s_timer_hits);
    CHECK(g_mock.timer_armed_us == 10000, "timer armed for 10 ms (10000 us)");
    g_mock.timer_cb(NULL);
    run_task(g_mock.irq_task, 1);
    CHECK(s_timer_hits == 1 && s_timer_ctx_seen, "timer callback ran once with its context");
    run_task(g_mock.irq_task, 1);
    CHECK(s_timer_hits == 1, "timer is one-shot");

    smtc_modem_hal_start_timer(10, timer_user_cb, &s_timer_hits);
    g_mock.timer_cb(NULL);
    smtc_modem_hal_stop_timer();
    run_task(g_mock.irq_task, 1);
    CHECK(s_timer_hits == 1, "stop_timer discards a fire that is already pending");

    smtc_modem_hal_start_timer(0, timer_user_cb, &s_timer_hits);
    CHECK(g_mock.timer_armed_us == 1, "0 ms timer is armed for 1 us, not left idle");

    smtc_modem_hal_start_timer(0xFFFFFFFFu, timer_user_cb, &s_timer_hits);
    CHECK(g_mock.timer_armed_us == 0xFFFFFFFFull * 1000ull, "49-day timer does not overflow");
}

int main(void)
{
    g_mock.fake_time_us = -1;
    g_mock.busy_pin = -1;
    printf("timer dispatch method in this build: %s\n",
#if CONFIG_ESP_TIMER_SUPPORT_ISR_DISPATCH_METHOD
           "ISR"
#else
           "TASK"
#endif
    );
    test_radio_hal();
    test_bsp();
    test_modem_hal();
    test_stack_and_dispatch();

    /* Keep every API symbol the stack needs reachable so the link step reports gaps. */
    volatile void *keep[] = { (void *)smtc_modem_init, (void *)smtc_modem_run_engine, (void *)lbm_esp32_lock,
                              (void *)lbm_esp32_unlock, (void *)lbm_esp32_wake };
    (void)keep;
    printf(s_fail ? "\n%d CHECK(S) FAILED\n" : "\nall checks passed\n", s_fail);
    return s_fail ? 1 : 0;
}
