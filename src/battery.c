#include "battery.h"
#include "app_config.h"   /* pulls board.h -> BOARD_BATTERY_* */
#include "bq27220.h"
#include <stdint.h>
#include "esp_timer.h"

bool battery_present(void)
{
#if defined(BOARD_BATTERY_GAUGE_I2C)
    /* Runtime rather than a board fact, unlike the other two backends: the
     * gauge is certainly fitted, but it NACKs while busy or unconfigured, and
     * publishing 0 mV in that window reads as a flat cell. See bq27220.h. */
    return bq27220_available();
#elif defined(BOARD_BATTERY_PMIC) || defined(BOARD_BATTERY_ADC_CHANNEL)
    return true;
#else
    return false;
#endif
}


/* ---------- per-board state-of-charge tables ----------
 *
 * Seeed's SenseCraft HMI firmware ships a measured 101-point voltage -> percent
 * table per reTerminal model (src/boards/reterminal_e100x/config.h in
 * Seeed-Projects/OSHW-reTerminal-Series-E-D, review 2026-09-11). Index is the
 * percent, value is the resting cell voltage in mV through the same 2:1
 * divider and eFuse-calibrated ADC path we use. All three top out near
 * 4.11 V, the charger's real termination point, so the generic 4.20 V line
 * below never reached 100 percent on these boards and under-read the middle
 * of the curve by about half (3.70 V: 30 percent generic, ~60 measured on
 * the E1001). E1001 and E1002 share a cell and a table. The E1001 table has
 * a step at 90 -> 91 (3927 -> 3984 mV); that is Seeed's data, kept verbatim. */
#if defined(TESSERAE_BOARD_SEEED_E1001) || defined(TESSERAE_BOARD_SEEED_E1001_GRAY) || \
    defined(TESSERAE_BOARD_SEEED_E1001_GRAY_LEGACY) || defined(TESSERAE_BOARD_SEEED_E1002)
#define BATTERY_SOC_TABLE 1
static const uint16_t s_soc_mv[101] = {
    2795, 2795, 2990, 3107, 3189, 3252, 3300, 3334, 3354, 3367,
    3378, 3389, 3399, 3409, 3418, 3426, 3434, 3442, 3449, 3457,
    3464, 3471, 3478, 3485, 3491, 3498, 3504, 3510, 3516, 3522,
    3528, 3534, 3540, 3546, 3552, 3558, 3564, 3569, 3575, 3581,
    3587, 3592, 3598, 3603, 3609, 3614, 3620, 3625, 3631, 3636,
    3642, 3647, 3653, 3658, 3664, 3669, 3675, 3680, 3686, 3691,
    3697, 3703, 3709, 3715, 3721, 3727, 3733, 3739, 3746, 3752,
    3759, 3765, 3772, 3779, 3786, 3793, 3801, 3808, 3816, 3824,
    3832, 3840, 3849, 3858, 3867, 3876, 3886, 3896, 3906, 3916,
    3927, 3984, 3995, 4007, 4019, 4032, 4045, 4059, 4074, 4090,
    4111,
};
#elif defined(TESSERAE_BOARD_SEEED_E1003)
#define BATTERY_SOC_TABLE 1
static const uint16_t s_soc_mv[101] = {
    3204, 3263, 3308, 3345, 3376, 3402, 3424, 3444, 3462, 3477,
    3492, 3505, 3517, 3528, 3539, 3550, 3560, 3570, 3579, 3588,
    3597, 3606, 3614, 3623, 3631, 3640, 3648, 3656, 3665, 3673,
    3682, 3691, 3700, 3709, 3718, 3727, 3736, 3745, 3754, 3764,
    3772, 3781, 3790, 3798, 3806, 3814, 3821, 3829, 3836, 3843,
    3849, 3856, 3862, 3868, 3874, 3880, 3885, 3890, 3895, 3900,
    3905, 3910, 3914, 3919, 3923, 3928, 3932, 3937, 3941, 3945,
    3949, 3954, 3958, 3962, 3966, 3971, 3975, 3980, 3984, 3989,
    3994, 3999, 4004, 4010, 4015, 4021, 4027, 4033, 4039, 4045,
    4051, 4057, 4063, 4069, 4075, 4081, 4087, 4093, 4098, 4104,
    4110,
};
#elif defined(TESSERAE_BOARD_SEEED_E1004)
#define BATTERY_SOC_TABLE 1
static const uint16_t s_soc_mv[101] = {
    3090, 3092, 3181, 3245, 3292, 3320, 3337, 3351, 3361, 3369,
    3377, 3384, 3392, 3402, 3411, 3421, 3430, 3438, 3446, 3454,
    3461, 3468, 3475, 3481, 3486, 3492, 3497, 3502, 3506, 3511,
    3515, 3519, 3523, 3527, 3531, 3535, 3538, 3542, 3546, 3550,
    3554, 3558, 3562, 3566, 3570, 3575, 3579, 3585, 3590, 3595,
    3601, 3607, 3613, 3620, 3627, 3634, 3643, 3651, 3660, 3670,
    3680, 3690, 3701, 3712, 3723, 3733, 3744, 3753, 3763, 3772,
    3781, 3790, 3798, 3806, 3814, 3822, 3831, 3841, 3851, 3861,
    3871, 3881, 3891, 3900, 3908, 3916, 3924, 3931, 3938, 3945,
    3954, 3965, 3976, 3987, 3998, 4010, 4022, 4034, 4047, 4061,
    4113,
};
#endif

#ifdef BATTERY_SOC_TABLE
/* Interpolate within the table; clamp outside it. */
static int soc_from_table(int mv)
{
    if (mv <= s_soc_mv[0])   return 0;
    if (mv >= s_soc_mv[100]) return 100;
    int lo = 0, hi = 100;
    while (hi - lo > 1) {            /* largest lo with s_soc_mv[lo] <= mv */
        int mid = (lo + hi) / 2;
        if (s_soc_mv[mid] <= mv) lo = mid; else hi = mid;
    }
    int span = s_soc_mv[hi] - s_soc_mv[lo];
    if (span <= 0) return hi;
    return lo + ((mv - s_soc_mv[lo]) * 2 + span) / (2 * span);   /* nearest */
}
#endif

int battery_pct(int mv)
{
    if (mv <= 0)    return 0;       /* unknown -> 0 is the safe report */
#ifdef BATTERY_SOC_TABLE
    return soc_from_table(mv);
#endif
#ifdef BOARD_BATTERY_NIMH_CELLS
    /* NiMH pack (paperlesspaper frames: 4 x AAA / AA). Per-cell curve: flat
     * around 1.2 V for most of the discharge, then a knee under 1.15 V; the
     * vendor cuts the frame off at 1.05 V/cell. Breakpoints are generic NiMH
     * resting-voltage figures, not a measured pack. */
    const int c = mv / BOARD_BATTERY_NIMH_CELLS;
    if (c >= 1400) return 100;
    if (c >= 1300) return 90 + (c - 1300) * 10 / 100;
    if (c >= 1250) return 70 + (c - 1250) * 20 / 50;
    if (c >= 1200) return 40 + (c - 1200) * 30 / 50;
    if (c >= 1150) return 20 + (c - 1150) * 20 / 50;
    if (c >= 1100) return 8 + (c - 1100) * 12 / 50;
    if (c >= 1050) return (c - 1050) * 8 / 50;
    return 0;
#else
    /* Two-segment piecewise linear: the Li-Po discharge curve is non-linear
     * below 3.7 V, so a single line over-reports remaining capacity. */
    if (mv >= 4200) return 100;
    if (mv <= 3300) return 0;
    if (mv >= 3700) return 30 + (mv - 3700) * 70 / 500;
    return (mv - 3300) * 30 / 400;
#endif
}

int battery_read_pct(void)
{
#if defined(BOARD_BATTERY_GAUGE_I2C)
    /* Prefer the gauge's own figure. It coulomb-counts, so it accounts for load
     * and for the pack's actual capacity; battery_pct() can only read a resting
     * voltage off a generic curve and is wrong under load in both directions. */
    return bq27220_battery_pct();
#else
    return battery_pct(battery_read_mv());
#endif
}

/* ---------- always-on eligibility (see battery.h for why it is shaped so) ---- */

bool power_battery_critical(void)
{
    /* No sense means no cell to protect, not "flat". Guarding on this is what
     * keeps a board with no divider (the XIAO C3 panel) from reading 0 mV and
     * dropping out of always-on immediately and permanently. */
    if (!battery_present()) return false;
    return battery_read_pct() < AWAKE_BATTERY_MIN_PCT;
}

bool power_can_stay_awake(void)
{
    /* Advertised by every board; the operator decides (see battery.h). A cell
     * we can actually see running down is the one thing that retracts it. */
    return !power_battery_critical();
}

#if defined(BOARD_BATTERY_GAUGE_I2C)

/* Gauge boards (reTerminal Sticky): a BQ27220 on I2C, no sense divider to any
 * ADC pin. The mV is the gauge's Voltage(); the percentage does NOT come
 * through battery_pct() -- see battery_read_pct() above. */

int battery_read_mv(void)
{
    return bq27220_battery_mv();
}

#elif defined(BOARD_BATTERY_PMIC)

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
/* A board whose effective ratio is not a whole number (a high-impedance
 * divider the ADC under-samples, calibrated against a meter) gives it as
 * hundredths instead. */
#ifdef BOARD_BATTERY_DIVIDER_X100
#  define BATTERY_SCALE(pin_mv)  ((pin_mv) * BOARD_BATTERY_DIVIDER_X100 / 100)
#else
#  define BATTERY_SCALE(pin_mv)  ((pin_mv) * BOARD_BATTERY_DIVIDER)
#endif
/* Load-switch polarity: most boards enable the divider with a HIGH; a P-FET
 * high-side switch (paperlesspaper OpenPaper 7) wants a LOW. */
#ifdef BOARD_VBAT_SWITCH_ACTIVE_LOW
#  define VBAT_SWITCH_ON   0
#  define VBAT_SWITCH_OFF  1
#else
#  define VBAT_SWITCH_ON   1
#  define VBAT_SWITCH_OFF  0
#endif

/* The first read of a wake happens before the radio is up (the goodbye gate
 * in main.c); later ones (status post, OTA gate, BLE) land under Wi-Fi TX
 * bursts that sag a small cell through the divider and read low, and each
 * costs a load-switch pulse plus an ADC unit init. Reuse the quiet reading
 * for BATTERY_CACHE_MS, long enough for a normal wake, short enough that a
 * long BLE session or always-on loop still sees fresh numbers. TRMNL samples
 * once before Wi-Fi and reuses it for the same reason. */
#ifndef BATTERY_CACHE_MS
#define BATTERY_CACHE_MS  60000
#endif
static int     s_cache_mv;
static int64_t s_cache_at_us = -1;

static int battery_sample_mv(void);

int battery_read_mv(void)
{
    int64_t now = esp_timer_get_time();
    if (s_cache_at_us >= 0 && s_cache_mv > 0 &&
        now - s_cache_at_us < (int64_t)BATTERY_CACHE_MS * 1000)
        return s_cache_mv;
    int mv = battery_sample_mv();
    if (mv > 0) { s_cache_mv = mv; s_cache_at_us = now; }
    return mv;
}

static int battery_sample_mv(void)
{
#ifdef BOARD_VBAT_SWITCH_PIN
    /* Some boards gate the sense divider behind a load switch to avoid a
     * constant drain; enable it around the read. */
    gpio_set_direction(BOARD_VBAT_SWITCH_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_VBAT_SWITCH_PIN, VBAT_SWITCH_ON);
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

    /* Calibration scheme is per-target: the S3 / C3 / C6 have curve fitting (a
     * factory polynomial in eFuse); the classic ESP32 (and S2) have only line
     * fitting off eFuse Two Point / Vref. Build whichever this target ships.
     * If the eFuse carries no calibration data, create fails and the read
     * falls through to 0 mV, which the server reads as "unknown". */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cc = {
        .unit_id = BOARD_BATTERY_ADC_UNIT, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    esp_err_t cali_err = adc_cali_create_scheme_curve_fitting(&cc, &cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t lc = {
        .unit_id = BOARD_BATTERY_ADC_UNIT, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    esp_err_t cali_err = adc_cali_create_scheme_line_fitting(&lc, &cali);
#else
#  error "battery.c: no ADC calibration scheme available for this target"
#endif
    if (cali_err != ESP_OK) {
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

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_delete_scheme_curve_fitting(cali);
#else
    adc_cali_delete_scheme_line_fitting(cali);
#endif
    adc_oneshot_del_unit(adc);

done:
#ifdef BOARD_VBAT_SWITCH_PIN
    gpio_set_level(BOARD_VBAT_SWITCH_PIN, VBAT_SWITCH_OFF);
#endif
    return BATTERY_SCALE(pin_mv);
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
    gpio_set_level(BOARD_VBAT_SWITCH_PIN, VBAT_SWITCH_ON);
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGW(T, "load switch GPIO%d driven to %d", BOARD_VBAT_SWITCH_PIN, VBAT_SWITCH_ON);
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
    /* Same per-target split as battery_read_mv() above: the classic ESP32 has
     * only line fitting, and a board being brought up is exactly where this
     * sweep gets used. */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cc = {
        .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    adc_cali_create_scheme_curve_fitting(&cc, &cali);
#else
    adc_cali_line_fitting_config_t lc = {
        .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    adc_cali_create_scheme_line_fitting(&lc, &cali);
#endif

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
