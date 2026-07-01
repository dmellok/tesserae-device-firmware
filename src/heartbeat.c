#include "heartbeat.h"
#include "app_config.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "hb";

/* Battery sense — copied from Waveshare's 01_ADC_Test reference for the
 * ESP32-S3-ePaper-13.3E6 board. Battery is routed through a 1:3 resistor
 * divider into GPIO8 / ADC1 channel 7. */
#define BATT_ADC_UNIT     ADC_UNIT_1
#define BATT_ADC_CHANNEL  ADC_CHANNEL_7
#define BATT_DIVIDER      3

/* Read the battery rail in millivolts. Returns 0 on any ADC failure --
 * the caller should treat 0 as "unknown" rather than "empty". */
static int read_battery_mv(void)
{
    adc_oneshot_unit_handle_t adc1 = NULL;
    adc_cali_handle_t         cali = NULL;
    int raw = 0, pin_mv = 0;

    adc_oneshot_unit_init_cfg_t init = { .unit_id = BATT_ADC_UNIT };
    if (adc_oneshot_new_unit(&init, &adc1) != ESP_OK) return 0;

    adc_oneshot_chan_cfg_t chan = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_config_channel(adc1, BATT_ADC_CHANNEL, &chan) != ESP_OK) {
        adc_oneshot_del_unit(adc1);
        return 0;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BATT_ADC_UNIT,
        .atten   = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali) != ESP_OK) {
        adc_oneshot_del_unit(adc1);
        return 0;
    }

    int sum = 0, ok = 0;
    /* 8-sample mean smooths out the noise from the divider / panel-rail
     * coupling without adding meaningful wake-time latency. */
    for (int i = 0; i < 8; i++) {
        if (adc_oneshot_read(adc1, BATT_ADC_CHANNEL, &raw) == ESP_OK) {
            int mv = 0;
            if (adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) {
                sum += mv;
                ok++;
            }
        }
    }
    if (ok > 0) pin_mv = sum / ok;

    adc_cali_delete_scheme_curve_fitting(cali);
    adc_oneshot_del_unit(adc1);

    return pin_mv * BATT_DIVIDER;
}

/* Map a Li-Po cell voltage (mV) to a 0-100% state-of-charge estimate.
 * Two-segment piecewise linear -- the discharge curve is non-linear
 * below 3.7 V so a single line over-reports remaining capacity. */
static int mv_to_pct(int mv)
{
    if (mv <= 0)    return 0;       /* unknown -> 0 is the safe report */
    if (mv >= 4200) return 100;
    if (mv <= 3300) return 0;
    if (mv >= 3700) return 30 + (mv - 3700) * 70 / 500;
    return (mv - 3300) * 30 / 400;
}

static int current_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

/* Short-string mapping of the reset reason so the server can distinguish
 * a normal timer wake ("timer") from a brownout, panic, watchdog, or repeated
 * power-on -- the latter three signal real trouble on battery. */
static const char *wake_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "timer";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        case ESP_RST_USB:       return "usb";
        case ESP_RST_JTAG:      return "jtag";
        default:                return "unknown";
    }
}

void heartbeat_format_json(char *dst, size_t dst_sz,
                           int sleep_interval_s,
                           esp_reset_reason_t reset_reason,
                           time_t sleep_until)
{
    if (!dst || dst_sz == 0) return;

    int mv   = read_battery_mv();
    int pct  = mv_to_pct(mv);
    int rssi = current_rssi();
    char ip[16] = {0};
    wifi_manager_get_sta_ip(ip, sizeof(ip));
    const char *wake = wake_reason_str(reset_reason);

    ESP_LOGI(TAG, "battery=%d mV (%d%%), rssi=%d dBm, ip=%s, fw=%s, "
                  "sleep=%ds, wake=%s, sleep_until=%lld",
             mv, pct, rssi, ip, FW_VERSION, sleep_interval_s, wake,
             (long long)sleep_until);

    /* Base payload: all unconditional fields. next_sleep_s mirrors
     * sleep_interval_s for this firmware (we always sleep for the configured
     * interval); the two keys exist because Tesserae's smart-sync wire
     * contract distinguishes "configured cadence" from "actual upcoming
     * sleep" -- firmwares that dynamically extend sleep on low battery
     * could publish different values. */
    int n = snprintf(dst, dst_sz,
             "{\"battery_mv\":%d,\"battery_pct\":%d,\"rssi\":%d,"
             "\"ip\":\"%s\",\"fw_version\":\"%s\","
             "\"kind\":\"esp32_client\",\"panel_w\":%d,\"panel_h\":%d,"
             "\"sleep_interval_s\":%d,\"next_sleep_s\":%d,"
             "\"wake_reason\":\"%s\"",
             mv, pct, rssi, ip, FW_VERSION, EPD_WIDTH, EPD_HEIGHT,
             sleep_interval_s, sleep_interval_s, wake);

    if (n < 0 || (size_t)n >= dst_sz) return;   /* truncated; leave as-is */

    /* sleep_until is optional -- omit when NTP isn't synced so the server
     * falls back to its tolerance-window math instead of seeing 0/garbage. */
    if (sleep_until > 0) {
        int m = snprintf(dst + n, dst_sz - n,
                         ",\"sleep_until\":%lld", (long long)sleep_until);
        if (m > 0 && (size_t)(n + m) < dst_sz) n += m;
    }

    if ((size_t)n < dst_sz - 1) {
        dst[n]     = '}';
        dst[n + 1] = '\0';
    } else if (dst_sz > 0) {
        dst[dst_sz - 1] = '\0';
    }
}
