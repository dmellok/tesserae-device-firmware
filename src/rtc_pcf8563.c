/* rtc_pcf8563.c -- NXP PCF8563 real-time clock. See rtc_pcf8563.h. */

#include "rtc_pcf8563.h"

#ifdef BOARD_HAS_PCF8563

#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "i2c_bus.h"

/* Register map, from examples/base/RTC_PCF8563/RTC_PCF8563.ino. Every time
 * field is BCD except the weekday. */
#define PCF_REG_CTRL1     0x00   /* bit 5 STOP halts the oscillator */
#define PCF_REG_CTRL2     0x01   /* alarm / timer flags and enables */
#define PCF_REG_SECONDS   0x02   /* bit 7 VL: backup ran low, time unreliable */
#define PCF_REG_CLKOUT    0x0D   /* bit 7 FE enables the CLKOUT pin */

#define PCF_VL_BIT        0x80
#define PCF_CENTURY_BIT   0x80   /* in the months register: 1 = 1900s */
#define PCF_TIME_REGS     7      /* seconds .. years, burst from 0x02 */

#define PCF_TIMEOUT_MS    50

/* A stored year below this is a clock that was never set (the chip powers up
 * at 2000-01-01), so it must not seed the system clock. */
#define PCF_MIN_YEAR      2025

static const char *TAG = "pcf8563";

static i2c_master_dev_handle_t s_dev;
static bool s_probed;
static bool s_present;

static inline uint8_t bcd_to_dec(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0FU));
}

static inline uint8_t dec_to_bcd(uint8_t dec)
{
    return (uint8_t)(((dec / 10U) << 4) | (dec % 10U));
}

static bool rtc_open(void)
{
    if (s_dev != NULL) return true;

    /* Port 0 is shared with the SHT4x (and the GT911 on the E1003), so the bus
     * must come from the get-or-create helper, never i2c_new_master_bus(). */
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_bus_get(BOARD_PCF8563_I2C_PORT, BOARD_PCF8563_I2C_SDA,
                    BOARD_PCF8563_I2C_SCL, &bus) != ESP_OK) {
        return false;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_PCF8563_I2C_ADDR,
        .scl_speed_hz    = BOARD_PCF8563_I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev) == ESP_OK;
}

static bool rtc_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t tx[2] = { reg, value };
    return i2c_master_transmit(s_dev, tx, sizeof tx, PCF_TIMEOUT_MS) == ESP_OK;
}

static bool rtc_read_regs(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len,
                                       PCF_TIMEOUT_MS) == ESP_OK;
}

/* Probe = read CTRL1. A bare address probe would do, but a register read also
 * proves the bus is not wedged, which is the failure a NACK-only probe hides. */
bool rtc_pcf8563_present(void)
{
    if (s_probed) return s_present;
    s_probed = true;

    if (!rtc_open()) {
        ESP_LOGW(TAG, "I2C bus unavailable");
        return false;
    }
    uint8_t ctrl1 = 0;
    s_present = rtc_read_regs(PCF_REG_CTRL1, &ctrl1, 1);
    if (!s_present) ESP_LOGW(TAG, "no answer at 0x%02x", BOARD_PCF8563_I2C_ADDR);
    return s_present;
}

/* Seeed's rtcInit(): oscillator running, flags cleared, CLKOUT off. CLKOUT
 * would otherwise toggle at 32 kHz forever on the coin cell. */
static bool rtc_init_regs(void)
{
    return rtc_write_reg(PCF_REG_CTRL1, 0x00) &&
           rtc_write_reg(PCF_REG_CTRL2, 0x00) &&
           rtc_write_reg(PCF_REG_CLKOUT, 0x00);
}

/* Days since 1970-01-01 for a proleptic Gregorian civil date. Avoids timegm(),
 * which newlib does not promise, and mktime(), which would apply the TZ. */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

bool rtc_pcf8563_seed_clock(void)
{
    if (!rtc_pcf8563_present()) return false;

    if (!rtc_init_regs()) {
        ESP_LOGW(TAG, "init write failed");
        return false;
    }

    uint8_t raw[PCF_TIME_REGS];
    if (!rtc_read_regs(PCF_REG_SECONDS, raw, sizeof raw)) {
        ESP_LOGW(TAG, "time read failed");
        return false;
    }
    if (raw[0] & PCF_VL_BIT) {
        ESP_LOGW(TAG, "VL set: backup ran low, stored time not trusted");
        return false;
    }

    const unsigned sec   = bcd_to_dec(raw[0] & 0x7F);
    const unsigned min   = bcd_to_dec(raw[1] & 0x7F);
    const unsigned hour  = bcd_to_dec(raw[2] & 0x3F);
    const unsigned day   = bcd_to_dec(raw[3] & 0x3F);
    const unsigned month = bcd_to_dec(raw[5] & 0x1F);
    const int year = ((raw[5] & PCF_CENTURY_BIT) ? 1900 : 2000) + bcd_to_dec(raw[6]);

    if (year < PCF_MIN_YEAR || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59) {
        ESP_LOGW(TAG, "stored %04d-%02u-%02u %02u:%02u:%02u not plausible, ignored",
                 year, month, day, hour, min, sec);
        return false;
    }

    struct timeval tv = {
        .tv_sec  = (time_t)(days_from_civil(year, month, day) * 86400 +
                            hour * 3600 + min * 60 + sec),
        .tv_usec = 0,
    };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "settimeofday failed");
        return false;
    }
    ESP_LOGI(TAG, "system clock seeded: %04d-%02u-%02u %02u:%02u:%02u UTC",
             year, month, day, hour, min, sec);
    return true;
}

bool rtc_pcf8563_store_clock(void)
{
    if (!rtc_pcf8563_present()) return false;

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return false;

    struct tm t;
    memset(&t, 0, sizeof t);
    const time_t now = tv.tv_sec;
    if (gmtime_r(&now, &t) == NULL) return false;

    const int year = t.tm_year + 1900;
    /* One century bit, and Seeed's convention is 0 = 2000s. Outside that window
     * the value could not be read back correctly, so refuse rather than wrap. */
    if (year < 2000 || year > 2099) {
        ESP_LOGW(TAG, "year %d outside 2000-2099, not stored", year);
        return false;
    }

    /* Burst write, same shape as Seeed's rtcSetTime(): the chip auto-increments
     * its address pointer, and writing seconds first resets its divider so the
     * new value starts counting cleanly. Weekday is plain binary, not BCD. */
    const uint8_t tx[1 + PCF_TIME_REGS] = {
        PCF_REG_SECONDS,
        dec_to_bcd((uint8_t)t.tm_sec),     /* VL cleared by the write */
        dec_to_bcd((uint8_t)t.tm_min),
        dec_to_bcd((uint8_t)t.tm_hour),
        dec_to_bcd((uint8_t)t.tm_mday),
        (uint8_t)t.tm_wday,
        dec_to_bcd((uint8_t)(t.tm_mon + 1)),   /* century bit 0 = 2000s */
        dec_to_bcd((uint8_t)(year % 100)),
    };
    if (i2c_master_transmit(s_dev, tx, sizeof tx, PCF_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "time write failed");
        return false;
    }
    ESP_LOGI(TAG, "stored %04d-%02d-%02d %02d:%02d:%02d UTC", year, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return true;
}

#endif /* BOARD_HAS_PCF8563 */
