/* m5pm1.c -- M5Stack M5PM1 PMIC, battery voltage. See m5pm1.h. */

#include "m5pm1.h"
#include "app_config.h"   /* pulls board.h -> BOARD_M5PM1_* */

#ifdef BOARD_BATTERY_M5PM1

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

#define REG_I2C_CFG   0x09   /* [4] 400 kHz, [3:0] idle-sleep timeout s (0 = never) */
#define REG_VBAT_L    0x22   /* mV low byte; 0x23 holds the high 4 bits */

#define PM1_TIMEOUT_MS  50

/* A 1S Li-Po the chip is genuinely measuring sits in this window; anything
 * else is a mangled transaction and is rejected rather than published. */
#define PM1_MV_MIN    2000
#define PM1_MV_MAX    5000

static const char *TAG = "m5pm1";

/* Survives deep sleep: one skipped wake must not turn a healthy pack into a
 * flat one. Cleared on a cold boot. */
RTC_DATA_ATTR static uint16_t s_mv;
RTC_DATA_ATTR static bool     s_valid;

static bool s_tried;
static i2c_master_dev_handle_t s_dev;

static bool pm1_open(void)
{
    if (s_dev != NULL) return true;
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_bus_get(BOARD_M5PM1_I2C_PORT, BOARD_M5PM1_I2C_SDA,
                    BOARD_M5PM1_I2C_SCL, &bus) != ESP_OK) {
        return false;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_M5PM1_I2C_ADDR,
        .scl_speed_hz    = BOARD_M5PM1_I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev) == ESP_OK;
}

static bool pm1_read_vbat(uint16_t *mv)
{
    uint8_t reg = REG_VBAT_L, rx[2];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, rx, sizeof rx, PM1_TIMEOUT_MS) != ESP_OK)
        return false;
    *mv = (uint16_t)rx[0] | ((uint16_t)(rx[1] & 0x0f) << 8);
    return true;
}

/* The PM1 drops its I2C interface into a sleep after an idle timeout and
 * NACKs the first transaction that follows. M5's reference code wakes it by
 * writing the idle timeout to 0 twice (the first write can be swallowed on
 * the way out of sleep); do the same before retrying. */
static void pm1_wake(void)
{
    uint8_t tx[2] = { REG_I2C_CFG, 0x00 };
    for (int i = 0; i < 2; i++) {
        (void)i2c_master_transmit(s_dev, tx, sizeof tx, PM1_TIMEOUT_MS);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void pm1_refresh(void)
{
    if (s_tried) return;
    s_tried = true;

    if (!pm1_open()) {
        ESP_LOGW(TAG, "I2C bus unavailable");
        return;
    }

    uint16_t mv = 0;
    bool ok = pm1_read_vbat(&mv);
    if (!ok) {
        pm1_wake();
        ok = pm1_read_vbat(&mv);
    }
    if (!ok) {
        ESP_LOGW(TAG, "VBAT read failed%s", s_valid ? "; using last sample" : "");
        return;
    }
    if (mv < PM1_MV_MIN || mv > PM1_MV_MAX) {
        ESP_LOGW(TAG, "VBAT %u mV out of range; ignored", (unsigned)mv);
        return;
    }
    s_mv = mv;
    s_valid = true;
    ESP_LOGI(TAG, "VBAT %u mV", (unsigned)mv);
}

bool m5pm1_battery_available(void)
{
    pm1_refresh();
    return s_valid;
}

int m5pm1_battery_mv(void)
{
    pm1_refresh();
    return s_valid ? (int)s_mv : 0;
}

#endif /* BOARD_BATTERY_M5PM1 */
