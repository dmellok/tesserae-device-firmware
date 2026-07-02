#include "battery.h"
#include "app_config.h"   /* pulls board.h -> BOARD_BATTERY_* */

int battery_pct(int mv)
{
    /* Two-segment piecewise linear: the Li-Po discharge curve is non-linear
     * below 3.7 V, so a single line over-reports remaining capacity. */
    if (mv <= 0)    return 0;       /* unknown -> 0 is the safe report */
    if (mv >= 4200) return 100;
    if (mv <= 3300) return 0;
    if (mv >= 3700) return 30 + (mv - 3700) * 70 / 500;
    return (mv - 3300) * 30 / 400;
}

#if defined(BOARD_BATTERY_PMIC)

/* PMIC boards (Waveshare PhotoPainter): battery comes from the AXP2101 fuel
 * gauge over I2C, not an ADC divider. pmic_init() is idempotent, so calling it
 * here makes the status-post read work regardless of whether the panel driver
 * has brought the PMIC up yet. battery_pct() maps the mV as for any board. */
#include "pmic.h"

int battery_read_mv(void)
{
    pmic_init();
    return (int) pmic_battery_mv();
}

#elif defined(BOARD_BATTERY_ADC_CHANNEL)

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef BOARD_BATTERY_ADC_UNIT
#  define BOARD_BATTERY_ADC_UNIT   ADC_UNIT_1
#endif
#ifndef BOARD_BATTERY_DIVIDER
#  define BOARD_BATTERY_DIVIDER     3     /* 1:3 resistor divider on the sense pin */
#endif

int battery_read_mv(void)
{
#ifdef BOARD_VBAT_SWITCH_PIN
    /* Some boards gate the sense divider behind a load switch to avoid a
     * constant drain; enable it around the read. */
    gpio_set_direction(BOARD_VBAT_SWITCH_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_VBAT_SWITCH_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));   /* let the load switch + divider settle */
#endif

    adc_oneshot_unit_handle_t adc = NULL;
    adc_cali_handle_t         cali = NULL;
    int raw = 0, pin_mv = 0;

    adc_oneshot_unit_init_cfg_t init = { .unit_id = BOARD_BATTERY_ADC_UNIT };
    if (adc_oneshot_new_unit(&init, &adc) != ESP_OK) goto done;

    adc_oneshot_chan_cfg_t chan = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    if (adc_oneshot_config_channel(adc, BOARD_BATTERY_ADC_CHANNEL, &chan) != ESP_OK) {
        adc_oneshot_del_unit(adc); adc = NULL; goto done;
    }

    adc_cali_curve_fitting_config_t cc = {
        .unit_id = BOARD_BATTERY_ADC_UNIT, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cc, &cali) != ESP_OK) {
        adc_oneshot_del_unit(adc); adc = NULL; goto done;
    }

    int sum = 0, ok = 0;   /* 8-sample mean smooths divider/panel-rail noise */
    for (int i = 0; i < 8; i++) {
        if (adc_oneshot_read(adc, BOARD_BATTERY_ADC_CHANNEL, &raw) == ESP_OK) {
            int mv = 0;
            if (adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) { sum += mv; ok++; }
        }
    }
    if (ok > 0) pin_mv = sum / ok;

    adc_cali_delete_scheme_curve_fitting(cali);
    adc_oneshot_del_unit(adc);

done:
#ifdef BOARD_VBAT_SWITCH_PIN
    gpio_set_level(BOARD_VBAT_SWITCH_PIN, 0);
#endif
    return pin_mv * BOARD_BATTERY_DIVIDER;
}

#else  /* no battery sense configured for this board */

int battery_read_mv(void) { return 0; }

#endif

#ifdef BATTERY_DEBUG_SWEEP
/* Board-agnostic ADC1 channel sweep for battery-pin bring-up: logs raw +
 * calibrated mV for every ADC1 channel (GPIO1..10) in a loop. Self-contained --
 * available on ANY board when built with -DBATTERY_DEBUG_SWEEP, regardless of
 * whether a battery channel is configured. Drives BOARD_VBAT_SWITCH_PIN if the
 * board defines one. Called from main.c before networking; never returns. */
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void battery_debug_sweep(void)
{
    static const char *T = "BATSWEEP";
#ifdef BOARD_VBAT_SWITCH_PIN
    gpio_set_direction(BOARD_VBAT_SWITCH_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_VBAT_SWITCH_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGW(T, "load switch GPIO%d driven HIGH", BOARD_VBAT_SWITCH_PIN);
#endif
#ifdef BATTERY_SWEEP_ENABLE_PINS
    /* Probe: drive candidate battery-divider enable pins HIGH before sweeping,
     * in case the sense divider is gated behind a load switch. */
    { const int en[] = { BATTERY_SWEEP_ENABLE_PINS };
      for (unsigned i = 0; i < sizeof(en)/sizeof(en[0]); i++) {
          gpio_set_direction(en[i], GPIO_MODE_OUTPUT);
          gpio_set_level(en[i], 1);
          ESP_LOGW(T, "candidate enable GPIO%d driven HIGH", en[i]);
      }
      vTaskDelay(pdMS_TO_TICKS(30)); }
#endif
    adc_oneshot_unit_handle_t adc = NULL;
    adc_oneshot_unit_init_cfg_t init = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&init, &adc) != ESP_OK) { ESP_LOGE(T, "adc unit init failed"); return; }

    adc_cali_handle_t cali = NULL;
    adc_cali_curve_fitting_config_t cc = {
        .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    adc_cali_create_scheme_curve_fitting(&cc, &cali);

    ESP_LOGW(T, "sweeping ADC1 ch0..9 (GPIO1..10), atten=12dB. A valid 1S cell "
                "reads pin*2 in 3300-4200mV; 2S reads pin*3/4 in 6000-8400mV.");
    while (1) {
        for (int ch = 0; ch <= 9; ch++) {
            adc_oneshot_chan_cfg_t chan = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
            if (adc_oneshot_config_channel(adc, ch, &chan) != ESP_OK) continue;
            int raw = 0, sum = 0, ok = 0, last = 0;
            for (int i = 0; i < 8; i++) {
                if (adc_oneshot_read(adc, ch, &raw) == ESP_OK) {
                    last = raw; int m = 0;
                    if (adc_cali_raw_to_voltage(cali, raw, &m) == ESP_OK) { sum += m; ok++; }
                }
            }
            int mv = ok ? sum / ok : -1;
            ESP_LOGI(T, "ch%d GPIO%2d: raw=%4d pin=%4dmV | x2=%5d x3=%5d x4=%5d",
                     ch, ch + 1, last, mv, mv * 2, mv * 3, mv * 4);
        }
        ESP_LOGI(T, "-------------------------------------------");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#endif
