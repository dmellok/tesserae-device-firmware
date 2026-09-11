/* sy6974.c -- Silergy SY6974 charger status. See sy6974.h. */

#include "sy6974.h"

#ifdef BOARD_HAS_SY6974

#include <stddef.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

#define SY_REG08_STATUS       0x08
#define SY_REG09_FAULT        0x09

#define SY_VBUS_STAT_SHIFT    5      /* REG08 bits 7:5 */
#define SY_VBUS_STAT_MASK     0x07
#define SY_CHRG_STAT_SHIFT    3      /* REG08 bits 4:3 */
#define SY_CHRG_STAT_MASK     0x03
#define SY_CHRG_NOT_CHARGING  0
#define SY_CHRG_PRECHARGE     1
#define SY_CHRG_FAST          2
#define SY_CHRG_DONE          3

#define SY_BAT_FAULT_BIT      0x08   /* REG09 bit 3 */
#define SY_NTC_FAULT_MASK     0x07   /* REG09 bits 2:0 */

#define SY_TIMEOUT_MS         50

static const char *TAG = "sy6974";

static i2c_master_dev_handle_t s_dev;
static bool s_probed;
static bool s_present;

static bool charger_open(void)
{
    if (s_dev != NULL) return true;

    /* Always the board's own port/pins. On the E1001 / E1002 this is port 1
     * (SDA 39 / SCL 40), a bus nothing else in the firmware uses, and passing
     * the sensor bus here instead would probe 0x6B on the wrong wires and
     * report the charger absent. The helper still owns the bus either way. */
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_bus_get(BOARD_SY6974_I2C_PORT, BOARD_SY6974_I2C_SDA,
                    BOARD_SY6974_I2C_SCL, &bus) != ESP_OK) {
        return false;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_SY6974_I2C_ADDR,
        .scl_speed_hz    = BOARD_SY6974_I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev) == ESP_OK;
}

/* Write-register / repeated-start / read-one-byte, the shape Seeed's
 * SY6974::readRegister() uses. */
static bool charger_read_reg(uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, 1,
                                       SY_TIMEOUT_MS) == ESP_OK;
}

bool sy6974_present(void)
{
    if (s_probed) return s_present;
    s_probed = true;

    if (!charger_open()) {
        ESP_LOGW(TAG, "I2C bus unavailable");
        return false;
    }
    uint8_t reg08 = 0;
    s_present = charger_read_reg(SY_REG08_STATUS, &reg08);
    if (!s_present) ESP_LOGW(TAG, "no answer at 0x%02x", BOARD_SY6974_I2C_ADDR);
    return s_present;
}

bool sy6974_read(sy6974_status_t *out)
{
    if (out == NULL) return false;
    if (!sy6974_present()) return false;

    /* REG09 is a LATCHED fault register: the first read returns faults
     * accumulated since the last read, the second the live state (BQ25601
     * family behaviour; Seeed's pmic_sy6974.cpp reads it twice with a 5 ms
     * gap for the same reason). A single read on an E1001 with a healthy
     * cell showed a stale BAT_FAULT (bench 2026-09-11). */
    uint8_t reg08 = 0, reg09 = 0, stale = 0;
    if (!charger_read_reg(SY_REG09_FAULT, &stale)) {
        ESP_LOGW(TAG, "status read failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    if (!charger_read_reg(SY_REG08_STATUS, &reg08) ||
        !charger_read_reg(SY_REG09_FAULT, &reg09)) {
        ESP_LOGW(TAG, "status read failed");
        return false;
    }
    if (stale != reg09) ESP_LOGD(TAG, "REG09 latched 0x%02x, live 0x%02x", stale, reg09);

    const uint8_t vbus = (uint8_t)((reg08 >> SY_VBUS_STAT_SHIFT) & SY_VBUS_STAT_MASK);
    const uint8_t chrg = (uint8_t)((reg08 >> SY_CHRG_STAT_SHIFT) & SY_CHRG_STAT_MASK);

    out->vbus_present  = vbus != 0;
    out->charging      = chrg == SY_CHRG_PRECHARGE || chrg == SY_CHRG_FAST;
    out->charge_done   = chrg == SY_CHRG_DONE;
    out->battery_fault = (reg09 & SY_BAT_FAULT_BIT) != 0 ||
                         (reg09 & SY_NTC_FAULT_MASK) != 0;
    out->reg08 = reg08;
    out->reg09 = reg09;

    ESP_LOGD(TAG, "REG08=0x%02x REG09=0x%02x vbus=%d charging=%d done=%d fault=%d",
             reg08, reg09, out->vbus_present, out->charging, out->charge_done,
             out->battery_fault);
    return true;
}

#endif /* BOARD_HAS_SY6974 */
