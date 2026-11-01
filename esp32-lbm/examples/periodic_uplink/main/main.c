/*
 * Join a LoRaWAN network (OTAA) and send a 4-byte counter every
 * CONFIG_EXAMPLE_UPLINK_PERIOD_S seconds. Based on the flow of Semtech's
 * main_periodical_uplink.c example.
 *
 * The region chosen here must also be enabled in the component's menuconfig
 * (LoRa Basics Modem -> Stack features).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lbm_esp32.h"
#include "smtc_modem_api.h"

static const char *TAG = "example";

#define STACK_ID 0

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

static uint32_t s_uplink_counter;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Parse exactly 2*len hex characters; returns false on any malformed input. */
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

static void send_counter(uint8_t fport)
{
    uint8_t buf[4] = {
        (uint8_t)(s_uplink_counter >> 24), (uint8_t)(s_uplink_counter >> 16),
        (uint8_t)(s_uplink_counter >> 8),  (uint8_t)(s_uplink_counter),
    };
    check(smtc_modem_request_uplink(STACK_ID, fport, false, buf, sizeof(buf)), "request_uplink");
    s_uplink_counter++;
}

/* Runs in the modem task with the stack lock held: call smtc_modem_* directly, never block. */
static void modem_event_callback(void)
{
    smtc_modem_event_t event;
    uint8_t            pending = 0;

    do {
        if (smtc_modem_get_event(&event, &pending) != SMTC_MODEM_RC_OK) {
            return;
        }
        switch (event.event_type) {
        case SMTC_MODEM_EVENT_RESET: {
            ESP_LOGI(TAG, "event: RESET");
            uint8_t dev_eui[SMTC_MODEM_EUI_LENGTH];
            uint8_t join_eui[SMTC_MODEM_EUI_LENGTH];
            uint8_t nwk_key[SMTC_MODEM_KEY_LENGTH];
            uint8_t app_key[SMTC_MODEM_KEY_LENGTH];
            if (!parse_hex(CONFIG_EXAMPLE_DEV_EUI, dev_eui, sizeof(dev_eui)) ||
                !parse_hex(CONFIG_EXAMPLE_JOIN_EUI, join_eui, sizeof(join_eui)) ||
                !parse_hex(CONFIG_EXAMPLE_NWK_KEY, nwk_key, sizeof(nwk_key)) ||
                !parse_hex(CONFIG_EXAMPLE_APP_KEY, app_key, sizeof(app_key))) {
                ESP_LOGE(TAG, "credentials in menuconfig are not valid hex of the right length");
                return;
            }
            check(smtc_modem_set_deveui(STACK_ID, dev_eui), "set_deveui");
            check(smtc_modem_set_joineui(STACK_ID, join_eui), "set_joineui");
            check(smtc_modem_set_nwkkey(STACK_ID, nwk_key), "set_nwkkey");
            check(smtc_modem_set_appkey(STACK_ID, app_key), "set_appkey");
            check(smtc_modem_set_region(STACK_ID, EXAMPLE_REGION), "set_region");
            check(smtc_modem_join_network(STACK_ID), "join_network");
            break;
        }
        case SMTC_MODEM_EVENT_JOINED:
            ESP_LOGI(TAG, "event: JOINED");
            send_counter(101);
            check(smtc_modem_alarm_start_timer(CONFIG_EXAMPLE_UPLINK_PERIOD_S), "alarm_start_timer");
            break;
        case SMTC_MODEM_EVENT_ALARM:
            send_counter(101);
            check(smtc_modem_alarm_start_timer(CONFIG_EXAMPLE_UPLINK_PERIOD_S), "alarm_start_timer");
            break;
        case SMTC_MODEM_EVENT_JOINFAIL:
            ESP_LOGW(TAG, "event: JOINFAIL (the stack retries on its own schedule)");
            break;
        case SMTC_MODEM_EVENT_TXDONE:
            ESP_LOGI(TAG, "event: TXDONE");
            break;
        case SMTC_MODEM_EVENT_DOWNDATA: {
            uint8_t                  payload[SMTC_MODEM_MAX_LORAWAN_PAYLOAD_LENGTH];
            uint8_t                  size = 0;
            uint8_t                  remaining = 0;
            smtc_modem_dl_metadata_t meta;
            if (smtc_modem_get_downlink_data(payload, &size, &meta, &remaining) == SMTC_MODEM_RC_OK) {
                ESP_LOGI(TAG, "event: DOWNDATA fport=%u, %u bytes", (unsigned)meta.fport, (unsigned)size);
                ESP_LOG_BUFFER_HEX(TAG, payload, size);
            }
            break;
        }
        default:
            ESP_LOGI(TAG, "event: type %d", (int)event.event_type);
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

    ESP_ERROR_CHECK(lbm_esp32_start(NULL, modem_event_callback));
    /* The modem task now runs the stack; app_main has nothing more to do. */
}
