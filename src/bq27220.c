/* bq27220.c -- TI BQ27220 I2C fuel gauge. See bq27220.h. */

#include "bq27220.h"
#include "app_config.h"   /* pulls board.h -> BOARD_BATTERY_GAUGE_* */

#ifdef BOARD_BATTERY_GAUGE_I2C

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "i2c_bus.h"

/* Standard commands. Every one of these is u16 little-endian, read as
 * write-register / repeated-start / read-two-bytes. */
#define BQ_VOLTAGE          0x08   /* battery voltage, mV */
#define BQ_STATE_OF_CHARGE  0x2c   /* state of charge, percent */
/* AverageCurrent() at 0x0c -- signed mA, positive meaning current INTO the
 * battery -- is where a charging indicator would come from on this board: the
 * Sticky's BQ25616 charger is not on I2C, so the gauge's own current sign is
 * the only reading available short of the CHARGE_STATE line on GPIO40. Nothing
 * publishes a charging flag today, so it is not read. */

#define BQ_TIMEOUT_MS       50

/* A 1S Li-Po that the gauge is genuinely tracking sits in this window. Anything
 * outside it is a mangled transaction or a gauge with no battery profile
 * loaded, not a cell, so it is rejected rather than published -- the same
 * "sanity-check before you trust it" the E1001 OTP probe applies to its
 * temperature read. */
#define BQ_MV_MIN           2000
#define BQ_MV_MAX           5000

static const char *TAG = "bq27220";

/* Survives deep sleep. A gauge NACKs while it is busy, sealed or mid-update,
 * and one skipped wake must not turn a healthy pack into a flat one, so a
 * failed read falls back to the last good sample instead of reporting zero.
 * Cleared on a cold boot, which is correct: we would rather say nothing than
 * repeat a reading from before the battery was changed. */
RTC_DATA_ATTR static uint16_t s_mv;
RTC_DATA_ATTR static uint8_t  s_pct;
RTC_DATA_ATTR static bool     s_valid;

static bool s_tried;                        /* per boot: read at most once a wake */
static i2c_master_dev_handle_t s_dev;

static bool gauge_open(void)
{
    if (s_dev != NULL) return true;

    /* Shared with the SHT40 on this board (same port, same pins), so the bus
     * must come from the get-or-create helper, never i2c_new_master_bus(). */
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_bus_get(BOARD_BATTERY_GAUGE_PORT, BOARD_BATTERY_GAUGE_SDA,
                    BOARD_BATTERY_GAUGE_SCL, &bus) != ESP_OK) {
        return false;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_BATTERY_GAUGE_ADDR,
        .scl_speed_hz    = BOARD_BATTERY_GAUGE_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev) == ESP_OK;
}

static bool gauge_read16(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, rx, sizeof rx,
                                    BQ_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    *out = (uint16_t)rx[0] | ((uint16_t)rx[1] << 8);
    return true;
}

static void gauge_refresh(void)
{
    if (s_tried) return;
    s_tried = true;

    if (!gauge_open()) {
        ESP_LOGW(TAG, "I2C bus unavailable");
        return;
    }

    uint16_t mv = 0, soc = 0;
    if (!gauge_read16(BQ_VOLTAGE, &mv) || !gauge_read16(BQ_STATE_OF_CHARGE, &soc)) {
        ESP_LOGW(TAG, "no answer at 0x%02x%s", BOARD_BATTERY_GAUGE_ADDR,
                 s_valid ? " (keeping the last reading)" : "");
        return;
    }
    if (mv < BQ_MV_MIN || mv > BQ_MV_MAX || soc > 100) {
        ESP_LOGW(TAG, "implausible sample %u mV / %u%%, ignored", mv, soc);
        return;
    }

    s_mv    = mv;
    s_pct   = (uint8_t)soc;
    s_valid = true;
    ESP_LOGI(TAG, "%u mV, %u%%", mv, soc);
}

bool bq27220_available(void)   { gauge_refresh(); return s_valid; }
int  bq27220_battery_mv(void)  { gauge_refresh(); return s_valid ? s_mv  : 0; }
int  bq27220_battery_pct(void) { gauge_refresh(); return s_valid ? s_pct : 0; }

#else  /* no gauge on this board */

bool bq27220_available(void)   { return false; }
int  bq27220_battery_mv(void)  { return 0; }
int  bq27220_battery_pct(void) { return 0; }

#endif
