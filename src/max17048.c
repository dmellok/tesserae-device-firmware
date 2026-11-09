/* max17048.c -- optional MAX17048 I2C fuel gauge. See max17048.h. */

#include "max17048.h"
#include "app_config.h"   /* pulls board.h -> BOARD_MAX17048_* */

#ifdef BOARD_BATTERY_MAX17048

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "i2c_bus.h"

#define MAX_REG_VCELL     0x02
#define MAX_REG_SOC       0x04
#define MAX_REG_VERSION   0x08

/* Short on purpose. With no gauge fitted the pins idle high on the internal
 * pull-ups and the address phase NACKs in well under a millisecond; the
 * timeout only bounds a wedged bus, and it must not hold up a board that was
 * never given a gauge. */
#define MAX_TIMEOUT_MS    20

/* Same plausibility window as bq27220.c: outside it is a mangled transaction,
 * not a 1S cell. */
#define MAX_MV_MIN        2000
#define MAX_MV_MAX        5000

/* Missed wakes in a row before the last good sample is dropped. One miss is a
 * transient; several mean the gauge is gone, and repeating a stale reading
 * forever would be worse than reporting nothing. */
#define MAX_MISS_LIMIT    3

static const char *TAG = "max17048";

/* Survives deep sleep, cleared on a cold boot (see bq27220.c for why). */
RTC_DATA_ATTR static uint16_t s_mv;
RTC_DATA_ATTR static uint8_t  s_pct;
RTC_DATA_ATTR static bool     s_valid;
RTC_DATA_ATTR static uint8_t  s_misses;

static bool s_tried;                       /* per boot: read at most once a wake */
static i2c_master_dev_handle_t s_dev;

static bool gauge_open(void)
{
    if (s_dev != NULL) return true;

    i2c_master_bus_handle_t bus = NULL;
    if (i2c_bus_get(BOARD_MAX17048_PORT, BOARD_MAX17048_SDA,
                    BOARD_MAX17048_SCL, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus unavailable");
        return false;
    }

    /* Address-only probe first, so a board without the gauge never gets a
     * device handle and never logs a transaction error. */
    if (i2c_master_probe(bus, BOARD_MAX17048_ADDR, MAX_TIMEOUT_MS) != ESP_OK) {
        return false;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_MAX17048_ADDR,
        .scl_speed_hz    = BOARD_MAX17048_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev) == ESP_OK;
}

static bool gauge_read16(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, rx, sizeof rx,
                                    MAX_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    *out = (uint16_t)(((uint16_t)rx[0] << 8) | rx[1]);   /* big-endian */
    return true;
}

static void gauge_miss(void)
{
    if (!s_valid) return;
    if (++s_misses >= MAX_MISS_LIMIT) {
        ESP_LOGW(TAG, "no answer for %u wakes; dropping the last reading", s_misses);
        s_valid  = false;
        s_misses = 0;
    }
}

static void gauge_refresh(void)
{
    if (s_tried) return;
    s_tried = true;

    if (!gauge_open()) {
        if (s_valid) ESP_LOGW(TAG, "no answer at 0x%02x (keeping the last reading)",
                              BOARD_MAX17048_ADDR);
        else         ESP_LOGI(TAG, "no gauge at 0x%02x; battery not reported",
                              BOARD_MAX17048_ADDR);
        gauge_miss();
        return;
    }

    uint16_t ver = 0, vcell = 0, soc = 0;
    if (!gauge_read16(MAX_REG_VERSION, &ver) ||
        !gauge_read16(MAX_REG_VCELL, &vcell) ||
        !gauge_read16(MAX_REG_SOC, &soc)) {
        ESP_LOGW(TAG, "read failed at 0x%02x", BOARD_MAX17048_ADDR);
        gauge_miss();
        return;
    }
    /* Something else answering at 0x36 is not our gauge: report nothing. */
    if ((ver & 0xfff0) != 0x0010) {
        ESP_LOGW(TAG, "device at 0x%02x has VERSION 0x%04x, not a MAX17048; ignored",
                 BOARD_MAX17048_ADDR, ver);
        gauge_miss();
        return;
    }

    /* 78.125 uV/LSB = 5/64 mV. */
    uint32_t mv = ((uint32_t)vcell * 5u) / 64u;
    if (mv < MAX_MV_MIN || mv > MAX_MV_MAX) {
        ESP_LOGW(TAG, "implausible sample %u mV, ignored", (unsigned)mv);
        gauge_miss();
        return;
    }
    /* High byte is whole percent, low byte 1/256. Round, then clamp: the
     * register can read above 100 before the model settles. */
    uint32_t pct = ((uint32_t)soc + 128u) >> 8;
    if (pct > 100) pct = 100;

    s_mv     = (uint16_t)mv;
    s_pct    = (uint8_t)pct;
    s_valid  = true;
    s_misses = 0;
    ESP_LOGI(TAG, "%u mV, %u%% (version 0x%04x)", (unsigned)mv, (unsigned)pct, ver);
}

bool max17048_available(void)   { gauge_refresh(); return s_valid; }
int  max17048_battery_mv(void)  { gauge_refresh(); return s_valid ? s_mv  : 0; }
int  max17048_battery_pct(void) { gauge_refresh(); return s_valid ? s_pct : 0; }

#else  /* no MAX17048 on this board */

bool max17048_available(void)   { return false; }
int  max17048_battery_mv(void)  { return 0; }
int  max17048_battery_pct(void) { return 0; }

#endif
