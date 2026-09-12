/* m5pm1.c -- M5Stack M5PM1 PMIC, battery voltage. See m5pm1.h. */

#include "m5pm1.h"
#include "app_config.h"   /* pulls board.h -> BOARD_M5PM1_* */

#if defined(BOARD_BATTERY_M5PM1) || defined(BOARD_FRONTLIGHT_M5PM1) || defined(BOARD_LED_M5PM1)

#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

#define REG_PWR_CFG   0x06   /* [4] LED_EN level, [3] BOOST, [2] LDO, [1] DCDC, [0] CHG */
#define REG_I2C_CFG   0x09   /* [4] 400 kHz, [3:0] idle-sleep timeout s (0 = never) */
#define REG_GPIO_MODE 0x10   /* [4:0] 1 = output */
#define REG_GPIO_DRV  0x13   /* [4:0] 1 = open-drain */
#define REG_GPIO_PUPD0 0x14  /* 2 bits per GPIO0-3: 00 none 01 up 10 down */
#define REG_GPIO_FUNC0 0x16  /* 2 bits per GPIO0-3: 00 gpio 01 irq 10 wake 11 special */
#define REG_VBAT_L    0x22   /* mV low byte; 0x23 holds the high 4 bits */
#define REG_PWM0_L    0x30   /* duty low byte; 0x31 = [5] POL [4] EN [3:0] duty high */
#define REG_PWM_FREQ_L 0x34  /* Hz, 16-bit little-endian */

#define PM1_LED_CTRL_BIT   (1u << 4)
#define PM1_FRONTLIGHT_GPIO 3
#define PM1_PWM_HZ         5000

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

static bool pm1_rd8(uint8_t reg, uint8_t *v)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, v, 1, PM1_TIMEOUT_MS) == ESP_OK;
}

static bool pm1_wr(uint8_t reg, const uint8_t *data, size_t n)
{
    uint8_t tx[4] = { reg };
    if (n > 3) return false;
    memcpy(tx + 1, data, n);
    return i2c_master_transmit(s_dev, tx, n + 1, PM1_TIMEOUT_MS) == ESP_OK;
}

static bool pm1_wr8(uint8_t reg, uint8_t v) { return pm1_wr(reg, &v, 1); }

/* Read-modify-write one register; a NACK gets one wake-and-retry. */
static bool pm1_rmw(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t cur = 0;
    if (!pm1_rd8(reg, &cur)) {
        pm1_wake();
        if (!pm1_rd8(reg, &cur)) return false;
    }
    uint8_t want = (uint8_t)((cur & ~mask) | (value & mask));
    if (want == cur) return true;
    return pm1_wr8(reg, want);
}

#ifdef BOARD_FRONTLIGHT_M5PM1
esp_err_t m5pm1_frontlight_set(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (!pm1_open()) return ESP_ERR_INVALID_STATE;

    /* Same order as M5's PowerDemo set_frontlight(): function "special"
     * (PWM0) on GPIO3, output, no pull, push-pull, then frequency and duty. */
    const uint8_t shift = PM1_FRONTLIGHT_GPIO * 2;
    bool ok = pm1_rmw(REG_GPIO_FUNC0, (uint8_t)(0x03 << shift), (uint8_t)(0x03 << shift))
           && pm1_rmw(REG_GPIO_MODE, (uint8_t)(1u << PM1_FRONTLIGHT_GPIO), (uint8_t)(1u << PM1_FRONTLIGHT_GPIO))
           && pm1_rmw(REG_GPIO_PUPD0, (uint8_t)(0x03 << shift), 0)
           && pm1_rmw(REG_GPIO_DRV, (uint8_t)(1u << PM1_FRONTLIGHT_GPIO), 0);
    const uint8_t freq[2] = { PM1_PWM_HZ & 0xff, PM1_PWM_HZ >> 8 };
    ok = ok && pm1_wr(REG_PWM_FREQ_L, freq, 2);
    const uint16_t duty12 = (uint16_t)((pct * 0x0FFF) / 100);
    const uint8_t duty[2] = { (uint8_t)(duty12 & 0xff),
                              (uint8_t)(((duty12 >> 8) & 0x0f) | (pct ? 0x10 : 0x00)) };
    ok = ok && pm1_wr(REG_PWM0_L, duty, 2);
    if (!ok) {
        ESP_LOGW(TAG, "frontlight %d%%: PMIC write failed", pct);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "frontlight %d%%", pct);
    return ESP_OK;
}
#endif

#ifdef BOARD_LED_M5PM1
esp_err_t m5pm1_led_set(bool on)
{
    if (!pm1_open()) return ESP_ERR_INVALID_STATE;
    return pm1_rmw(REG_PWR_CFG, PM1_LED_CTRL_BIT, on ? PM1_LED_CTRL_BIT : 0) ? ESP_OK : ESP_FAIL;
}
#endif

#ifdef BOARD_BATTERY_M5PM1
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

#endif /* BOARD_BATTERY_M5PM1 */
