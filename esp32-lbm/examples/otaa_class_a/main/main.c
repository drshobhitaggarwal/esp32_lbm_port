/*
 * LoRaWAN Class A device with OTAA activation and periodic uplinks.
 *
 * What it does
 *   1. On the first engine run the stack raises RESET. The handler loads the
 *      keys and region, selects Class A, and starts the join.
 *   2. On JOINED it applies the ADR profile and sends the first uplink, then
 *      starts an alarm timer. Each ALARM sends an uplink and restarts the timer.
 *   3. Downlinks (Class A: only in the two receive windows after an uplink) are
 *      logged. Port 2 switches the LED, port 3 sets a new uplink period.
 *   4. Optionally asks the network for a link check every N uplinks.
 *
 * The event handler runs in the modem task with the stack lock held: call
 * smtc_modem_* directly and never block. See README.md.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "lbm_esp32.h"
#include "smtc_modem_api.h"

static const char *TAG = "otaa_a";

#define STACK_ID 0

#define FPORT_LED    2
#define FPORT_PERIOD 3

#if defined(CONFIG_EXAMPLE_REGION_EU868)
#define EXAMPLE_REGION SMTC_MODEM_REGION_EU_868
#elif defined(CONFIG_EXAMPLE_REGION_AU915)
#define EXAMPLE_REGION SMTC_MODEM_REGION_AU_915
#elif defined(CONFIG_EXAMPLE_REGION_AS923_GRP1)
#define EXAMPLE_REGION SMTC_MODEM_REGION_AS_923_GRP1
#elif defined(CONFIG_EXAMPLE_REGION_IN865)
#define EXAMPLE_REGION SMTC_MODEM_REGION_IN_865
#elif defined(CONFIG_EXAMPLE_REGION_KR920)
#define EXAMPLE_REGION SMTC_MODEM_REGION_KR_920
#elif defined(CONFIG_EXAMPLE_REGION_RU864)
#define EXAMPLE_REGION SMTC_MODEM_REGION_RU_864
#else
#define EXAMPLE_REGION SMTC_MODEM_REGION_US_915
#endif

#if defined(CONFIG_EXAMPLE_ADR_MOBILE_LONG_RANGE)
#define EXAMPLE_ADR_PROFILE SMTC_MODEM_ADR_PROFILE_MOBILE_LONG_RANGE
#elif defined(CONFIG_EXAMPLE_ADR_MOBILE_LOW_POWER)
#define EXAMPLE_ADR_PROFILE SMTC_MODEM_ADR_PROFILE_MOBILE_LOW_POWER
#else
#define EXAMPLE_ADR_PROFILE SMTC_MODEM_ADR_PROFILE_NETWORK_CONTROLLED
#endif

#ifdef CONFIG_EXAMPLE_CONFIRMED
#define UPLINK_CONFIRMED true
#else
#define UPLINK_CONFIRMED false
#endif

static uint32_t s_uplink_counter;
static uint32_t s_period_s = CONFIG_EXAMPLE_UPLINK_PERIOD_S;

/* ------------------------------------------------------------------ helpers */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Parse exactly 2*len hex characters; false on any malformed input. */
static bool parse_hex(const char *s, uint8_t *out, size_t len)
{
    if (strlen(s) != 2 * len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        int hi = hex_nibble(s[2 * i]);
        int lo = hex_nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void check(smtc_modem_return_code_t rc, const char *what)
{
    if (rc != SMTC_MODEM_RC_OK) {
        ESP_LOGE(TAG, "%s failed (rc=%d)", what, (int)rc);
    }
}

static void led_set(int on)
{
#if CONFIG_EXAMPLE_LED_GPIO >= 0
    gpio_set_level(CONFIG_EXAMPLE_LED_GPIO, on ? 1 : 0);
#else
    (void)on;
#endif
}

static void led_init(void)
{
#if CONFIG_EXAMPLE_LED_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_EXAMPLE_LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(CONFIG_EXAMPLE_LED_GPIO, 0);
#endif
}

static bool is_joined(void)
{
    smtc_modem_status_mask_t status = 0;
    return smtc_modem_get_status(STACK_ID, &status) == SMTC_MODEM_RC_OK && (status & SMTC_MODEM_STATUS_JOINED) != 0;
}

/* ------------------------------------------------------------------ uplink */

/* Payload (8 bytes, big endian): uplink counter, seconds since boot. */
static void send_uplink(void)
{
    if (!is_joined()) {
        ESP_LOGW(TAG, "not joined, uplink skipped");
        return;
    }

    uint8_t max_len = 0;
    check(smtc_modem_get_next_tx_max_payload(STACK_ID, &max_len), "get_next_tx_max_payload");

    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    uint8_t  buf[8] = {
        (uint8_t)(s_uplink_counter >> 24), (uint8_t)(s_uplink_counter >> 16),
        (uint8_t)(s_uplink_counter >> 8),  (uint8_t)(s_uplink_counter),
        (uint8_t)(up_s >> 24),             (uint8_t)(up_s >> 16),
        (uint8_t)(up_s >> 8),              (uint8_t)(up_s),
    };
    size_t len = sizeof(buf);
    if (len > max_len) {
        /* The current data rate allows fewer bytes; send the counter only. */
        len = max_len >= 4 ? 4 : 0;
    }
    if (len == 0) {
        ESP_LOGW(TAG, "payload limit %u bytes is too small, uplink skipped", (unsigned)max_len);
        return;
    }

    smtc_modem_return_code_t rc =
        smtc_modem_request_uplink(STACK_ID, CONFIG_EXAMPLE_UPLINK_FPORT, UPLINK_CONFIRMED, buf, (uint8_t)len);
    check(rc, "request_uplink");
    if (rc == SMTC_MODEM_RC_OK) {
        ESP_LOGI(TAG, "uplink #%lu queued (%u bytes, limit %u)", (unsigned long)s_uplink_counter, (unsigned)len,
                 (unsigned)max_len);
        s_uplink_counter++;
#if CONFIG_EXAMPLE_LINK_CHECK_EVERY > 0
        if ((s_uplink_counter % CONFIG_EXAMPLE_LINK_CHECK_EVERY) == 0) {
            check(smtc_modem_trig_lorawan_mac_request(STACK_ID, SMTC_MODEM_LORAWAN_MAC_REQ_LINK_CHECK),
                  "trig_lorawan_mac_request");
        }
#endif
    }
}

static void start_alarm(void)
{
    check(smtc_modem_alarm_start_timer(s_period_s), "alarm_start_timer");
}

/* ---------------------------------------------------------------- downlink */

static void handle_downlink(void)
{
    uint8_t                  payload[SMTC_MODEM_MAX_LORAWAN_PAYLOAD_LENGTH];
    uint8_t                  size = 0;
    uint8_t                  remaining = 0;
    smtc_modem_dl_metadata_t meta;

    do {
        if (smtc_modem_get_downlink_data(payload, &size, &meta, &remaining) != SMTC_MODEM_RC_OK) {
            return;
        }
        /* rssi is dBm + 64, snr is in 0.25 dB steps. */
        ESP_LOGI(TAG, "downlink: fport %u, %u bytes, RX%u, rssi %d dBm, snr %d dB, DR%u", (unsigned)meta.fport,
                 (unsigned)size, (unsigned)meta.window, (int)meta.rssi - 64, (int)meta.snr / 4, (unsigned)meta.datarate);
        if (size > 0) {
            ESP_LOG_BUFFER_HEX(TAG, payload, size);
        }

        if (meta.fport == FPORT_LED && size >= 1) {
            led_set(payload[0] != 0);
            ESP_LOGI(TAG, "LED %s by downlink", payload[0] ? "on" : "off");
        } else if (meta.fport == FPORT_PERIOD && size >= 2) {
            uint32_t p = ((uint32_t)payload[0] << 8) | payload[1];
            if (p < 10) {
                p = 10;
            }
            s_period_s = p;
            ESP_LOGI(TAG, "uplink period set to %lu s by downlink", (unsigned long)s_period_s);
            start_alarm();
        }
    } while (remaining > 0);
}

/* ------------------------------------------------------------------- events */

static void on_reset(uint16_t count)
{
    ESP_LOGI(TAG, "RESET (reset count %u)", (unsigned)count);

    uint8_t dev_eui[SMTC_MODEM_EUI_LENGTH];
    uint8_t join_eui[SMTC_MODEM_EUI_LENGTH];
    uint8_t app_key[SMTC_MODEM_KEY_LENGTH];
    if (!parse_hex(CONFIG_EXAMPLE_DEV_EUI, dev_eui, sizeof(dev_eui)) ||
        !parse_hex(CONFIG_EXAMPLE_JOIN_EUI, join_eui, sizeof(join_eui)) ||
        !parse_hex(CONFIG_EXAMPLE_APP_KEY, app_key, sizeof(app_key))) {
        ESP_LOGE(TAG, "credentials in menuconfig are not valid hex of the right length");
        return;
    }

    check(smtc_modem_set_deveui(STACK_ID, dev_eui), "set_deveui");
    check(smtc_modem_set_joineui(STACK_ID, join_eui), "set_joineui");
    /* LoRaWAN 1.0.x: the AppKey is used for the join; LBM names it NwkKey. */
    check(smtc_modem_set_nwkkey(STACK_ID, app_key), "set_nwkkey");
    check(smtc_modem_set_appkey(STACK_ID, app_key), "set_appkey");
    check(smtc_modem_set_region(STACK_ID, EXAMPLE_REGION), "set_region");
    check(smtc_modem_set_class(STACK_ID, SMTC_MODEM_CLASS_A), "set_class");
    check(smtc_modem_join_network(STACK_ID), "join_network");
    ESP_LOGI(TAG, "joining (OTAA, Class A)");
}

static void on_joined(void)
{
    ESP_LOGI(TAG, "JOINED");
    led_set(1);
    /* The ADR profile can only be set once joined. */
    check(smtc_modem_adr_set_profile(STACK_ID, EXAMPLE_ADR_PROFILE, NULL), "adr_set_profile");
    send_uplink();
    start_alarm();
}

/* Runs in the modem task with the stack lock held. */
static void modem_event_callback(void)
{
    smtc_modem_event_t event;
    uint8_t            pending = 0;

    do {
        if (smtc_modem_get_event(&event, &pending) != SMTC_MODEM_RC_OK) {
            return;
        }
        switch (event.event_type) {
        case SMTC_MODEM_EVENT_RESET:
            on_reset(event.event_data.reset.count);
            break;
        case SMTC_MODEM_EVENT_JOINED:
            on_joined();
            break;
        case SMTC_MODEM_EVENT_JOINFAIL:
            ESP_LOGW(TAG, "JOINFAIL (the stack retries with growing back-off)");
            led_set(0);
            break;
        case SMTC_MODEM_EVENT_ALARM:
            send_uplink();
            start_alarm();
            break;
        case SMTC_MODEM_EVENT_TXDONE:
            switch (event.event_data.txdone.status) {
            case SMTC_MODEM_EVENT_TXDONE_CONFIRMED:
                ESP_LOGI(TAG, "TXDONE: sent, acknowledged by the network");
                break;
            case SMTC_MODEM_EVENT_TXDONE_SENT:
                ESP_LOGI(TAG, "TXDONE: sent");
                break;
            default:
                ESP_LOGW(TAG, "TXDONE: not sent or no acknowledgement");
                break;
            }
            break;
        case SMTC_MODEM_EVENT_DOWNDATA:
            handle_downlink();
            break;
        case SMTC_MODEM_EVENT_LINK_CHECK: {
            uint8_t margin = 0;
            uint8_t gateways = 0;
            if (event.event_data.link_check.status == SMTC_MODEM_EVENT_MAC_REQUEST_ANSWERED &&
                smtc_modem_get_lorawan_link_check_data(STACK_ID, &margin, &gateways) == SMTC_MODEM_RC_OK) {
                ESP_LOGI(TAG, "LINK_CHECK: margin %u dB, %u gateway(s)", (unsigned)margin, (unsigned)gateways);
            } else {
                ESP_LOGW(TAG, "LINK_CHECK: no answer");
            }
            break;
        }
        case SMTC_MODEM_EVENT_NO_DOWNLINK_THRESHOLD:
            ESP_LOGW(TAG, "no downlink received for a long time (ADR back-off)");
            break;
        default:
            ESP_LOGI(TAG, "event type %d", (int)event.event_type);
            break;
        }
    } while (pending > 0);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    led_init();
    ESP_ERROR_CHECK(lbm_esp32_start(NULL, modem_event_callback));
    /* The modem task now runs the stack; app_main has nothing more to do. */
}
