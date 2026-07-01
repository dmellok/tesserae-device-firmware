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

#ifdef BOARD_BATTERY_ADC_CHANNEL

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
