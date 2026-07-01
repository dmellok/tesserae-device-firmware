/* AXP2101 PMIC wrapper. See pmic.h for the contract.
 *
 * Ported verbatim from tesserae-photopainter-7.3-bin-client/src/pmic.c, which
 * in turn follows Waveshare's XPowersLib register map and the I2C bus-rescue
 * from aitjcize/esp32-photoframe. The whole body is gated on BOARD_HAS_PMIC so
 * ADC-battery boards link cheap no-op stubs (bottom of file) and pull in no
 * I2C. */

#include "pmic.h"
#include "app_config.h"          /* board.h -> BOARD_HAS_PMIC + PMIC_* pins */

#ifdef BOARD_HAS_PMIC

#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"         /* esp_rom_delay_us */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pmic";

/* AXP2101 register addresses (Waveshare XPowersLib AXP2101Constants.h). Only
 * the registers we touch are inlined. */
#define AXP2101_REG_STATUS1               0x00   /* battery present in bit 3 */
#define AXP2101_REG_ADC_DATA_RESULT0      0x34   /* VBAT high (5 bits)       */
#define AXP2101_REG_ADC_DATA_RESULT1      0x35   /* VBAT low  (8 bits)       */
#define AXP2101_REG_INPUT_CURRENT_LIMIT   0x16   /* VBUS current cap         */
#define AXP2101_REG_CHG_CURRENT           0x62   /* constant-current setting */
#define AXP2101_REG_CHG_TERMINATION       0x63   /* termination current      */
#define AXP2101_REG_LDO_ONOFF_CTRL0       0x90   /* ALDO1..4 in bits 0..3    */
#define AXP2101_REG_LDO_VOL0_CTRL         0x92   /* ALDO1 voltage            */
#define AXP2101_REG_LDO_VOL1_CTRL         0x93   /* ALDO2 voltage            */
#define AXP2101_REG_LDO_VOL2_CTRL         0x94   /* ALDO3 voltage            */
#define AXP2101_REG_LDO_VOL3_CTRL         0x95   /* ALDO4 voltage            */

/* ALDOn = 0.5V + N * 0.1V, encoded in the low 5 bits of LDO_VOLn_CTRL. */
#define ALDO_VOL_STEP_MV   100
#define ALDO_VOL_MIN_MV    500
#define ALDO_VOL_REG_3V3   ((3300 - ALDO_VOL_MIN_MV) / ALDO_VOL_STEP_MV)   /* 28 */

/* VBUS current limit (datasheet Table 7-16): 2000 mA == 0x06. */
#define VBUS_CURRENT_LIMIT_2A   0x06
/* Constant charge current 500 mA (low 5 bits of reg 0x62). */
#define CHG_CURRENT_500MA   0x09
/* Termination current 25 mA (low 4 bits of reg 0x63). */
#define CHG_TERMINATION_25MA   0x00

static i2c_master_bus_handle_t  s_i2c_bus  = NULL;
static i2c_master_dev_handle_t  s_axp2101  = NULL;
static bool                     s_inited   = false;

/* --------------------------- I2C plumbing --------------------------- */

static esp_err_t axp_read(uint8_t reg, uint8_t *out) {
    return i2c_master_transmit_receive(s_axp2101, &reg, 1, out, 1, 50);
}

static esp_err_t axp_write(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_axp2101, buf, sizeof(buf), 50);
}

/* Read-modify-write a 5-bit voltage selector, preserving the top 3 bits. */
static esp_err_t axp_set_voltage_reg(uint8_t reg, uint8_t code) {
    uint8_t v = 0;
    esp_err_t err = axp_read(reg, &v);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t nv = (uint8_t)((v & 0xE0) | (code & 0x1F));
    if (nv == v) {
        return ESP_OK;
    }
    return axp_write(reg, nv);
}

/* --------------------------- public API ----------------------------- */

esp_err_t pmic_init(void) {
    if (s_inited) {
        return ESP_OK;
    }

    /* Wake the AXP2101 by pulsing its IRQ pin LOW for >16 ms before touching
     * the bus. After some PMIC sleep states the chip powers its I2C interface
     * down; the IRQ pin is the documented wake source (REG26H[4]). Without
     * this, every transaction returns ESP_ERR_INVALID_STATE (no ACK). Waking
     * before the bus rescue means the chip sees a normal STOP rather than a
     * stream of phantom bits during its wake window. */
    {
        gpio_config_t irq_conf = {
            .pin_bit_mask = (1ULL << PMIC_PIN_IRQ),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&irq_conf);
        gpio_set_level(PMIC_PIN_IRQ, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(PMIC_PIN_IRQ, 1);
        vTaskDelay(pdMS_TO_TICKS(300));   /* generous post-wake settle */
        ESP_LOGI(TAG, "AXP2101 IRQ wake pulse done (pre-rescue)");
    }

    /* Manual I2C bus rescue: if a slave is mid-byte from a previous
     * power-cycle (SDA held LOW), the master can't START and every
     * transaction returns ESP_ERR_INVALID_STATE. Toggle SCL 9 times to clock
     * the stuck slave out, then issue a STOP. */
    {
        gpio_set_direction(PMIC_I2C_SCL, GPIO_MODE_OUTPUT_OD);
        gpio_set_direction(PMIC_I2C_SDA, GPIO_MODE_OUTPUT_OD);
        gpio_set_pull_mode(PMIC_I2C_SCL, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(PMIC_I2C_SDA, GPIO_PULLUP_ONLY);
        for (int i = 0; i < 9; i++) {
            gpio_set_level(PMIC_I2C_SCL, 0); esp_rom_delay_us(5);
            gpio_set_level(PMIC_I2C_SCL, 1); esp_rom_delay_us(5);
        }
        /* STOP: SDA low, SCL high, SDA high (LH transition). */
        gpio_set_level(PMIC_I2C_SDA, 0); esp_rom_delay_us(5);
        gpio_set_level(PMIC_I2C_SCL, 1); esp_rom_delay_us(5);
        gpio_set_level(PMIC_I2C_SDA, 1); esp_rom_delay_us(5);
        gpio_reset_pin(PMIC_I2C_SCL);
        gpio_reset_pin(PMIC_I2C_SDA);
        ESP_LOGI(TAG, "I2C bus rescue: 9 SCL pulses + STOP issued");
    }

    if (s_i2c_bus == NULL) {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port          = PMIC_I2C_PORT,
            .scl_io_num        = PMIC_I2C_SCL,
            .sda_io_num        = PMIC_I2C_SDA,
            .clk_source        = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags             = { .enable_internal_pullup = true },
        };
        esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (s_axp2101 == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = PMIC_AXP2101_ADDR,
            .scl_speed_hz    = PMIC_I2C_HZ,
        };
        esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_axp2101);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c add device failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    /* Probe: a successful STATUS1 read tells us the chip is alive. */
    uint8_t status = 0;
    esp_err_t err = axp_read(AXP2101_REG_STATUS1, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 not responding at 0x%02X: %s",
                 PMIC_AXP2101_ADDR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AXP2101 alive, STATUS1=0x%02X", status);

    /* Match Waveshare's reference init: cap VBUS at 2 A, charge 500 mA CC with
     * 25 mA termination. Leave DCDC1 (the SoC 3V3 rail) untouched. */
    axp_set_voltage_reg(AXP2101_REG_INPUT_CURRENT_LIMIT, VBUS_CURRENT_LIMIT_2A);
    axp_write(AXP2101_REG_CHG_CURRENT,     CHG_CURRENT_500MA);
    axp_write(AXP2101_REG_CHG_TERMINATION, CHG_TERMINATION_25MA);

    /* ALDO1..4 to 3.3 V (Waveshare's factory firmware enables all four
     * together at 3.3 V on every boot; the per-rail map isn't published). */
    axp_set_voltage_reg(AXP2101_REG_LDO_VOL0_CTRL, ALDO_VOL_REG_3V3);
    axp_set_voltage_reg(AXP2101_REG_LDO_VOL1_CTRL, ALDO_VOL_REG_3V3);
    axp_set_voltage_reg(AXP2101_REG_LDO_VOL2_CTRL, ALDO_VOL_REG_3V3);
    axp_set_voltage_reg(AXP2101_REG_LDO_VOL3_CTRL, ALDO_VOL_REG_3V3);

    /* Energise the analog rails before we drive any downstream data lines. */
    err = pmic_rails_set(true);
    if (err != ESP_OK) {
        return err;
    }

    s_inited = true;
    return ESP_OK;
}

esp_err_t pmic_rails_set(bool enabled) {
    /* ALDO1..4 -> bits 0..3 of LDO_ONOFF_CTRL0. Batch the write so the panel
     * and SD don't see staggered rail transitions. */
    uint8_t v = 0;
    esp_err_t err = axp_read(AXP2101_REG_LDO_ONOFF_CTRL0, &v);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t mask = 0x0F;   /* ALDO1..4 = bits 0..3 */
    uint8_t nv = enabled ? (v | mask) : (v & (uint8_t)~mask);
    if (nv == v) {
        return ESP_OK;
    }
    err = axp_write(AXP2101_REG_LDO_ONOFF_CTRL0, nv);
    if (err == ESP_OK && enabled) {
        vTaskDelay(pdMS_TO_TICKS(10));   /* rails settle before SPI */
    }
    return err;
}

uint16_t pmic_battery_mv(void) {
    if (!s_inited) {
        return 0;
    }
    uint8_t hi = 0, lo = 0;
    if (axp_read(AXP2101_REG_ADC_DATA_RESULT0, &hi) != ESP_OK) {
        return 0;
    }
    if (axp_read(AXP2101_REG_ADC_DATA_RESULT1, &lo) != ESP_OK) {
        return 0;
    }
    /* AXP2101 datasheet 7.4.1: VBAT = (H[4:0] << 8 | L) mV. */
    return (uint16_t)(((hi & 0x1F) << 8) | lo);
}

/* Piecewise-linear 1S LiPo VBAT -> SOC curve (light load). Finer breakpoints
 * across the noisy 3.7-3.9 V plateau. Not the AXP coulomb counter (reg 0xA4),
 * which needs a design-capacity write + learning cycle we don't perform. */
static const struct { uint16_t mv; uint8_t pct; } VBAT_CURVE[] = {
    { 4200, 100 }, { 4150, 95 }, { 4100, 90 }, { 4050, 85 },
    { 4000,  80 }, { 3950, 75 }, { 3900, 65 }, { 3850, 55 },
    { 3800,  45 }, { 3750, 35 }, { 3700, 25 }, { 3650, 15 },
    { 3600,   8 }, { 3550,  5 }, { 3500,  2 }, { 3400,  0 },
};

int pmic_battery_pct(void) {
    if (!s_inited) {
        return -1;
    }
    uint16_t mv = pmic_battery_mv();
    if (mv == 0) {
        return -1;
    }
    if (mv >= VBAT_CURVE[0].mv) {
        return VBAT_CURVE[0].pct;
    }
    const size_t n = sizeof(VBAT_CURVE) / sizeof(VBAT_CURVE[0]);
    if (mv <= VBAT_CURVE[n - 1].mv) {
        return VBAT_CURVE[n - 1].pct;
    }
    for (size_t i = 1; i < n; i++) {
        if (mv >= VBAT_CURVE[i].mv) {
            int span_mv  = VBAT_CURVE[i - 1].mv  - VBAT_CURVE[i].mv;
            int span_pct = VBAT_CURVE[i - 1].pct - VBAT_CURVE[i].pct;
            int over_mv  = mv - VBAT_CURVE[i].mv;
            return VBAT_CURVE[i].pct + (over_mv * span_pct + span_mv / 2) / span_mv;
        }
    }
    return 0;
}

bool pmic_battery_present(void) {
    if (!s_inited) {
        return false;
    }
    uint8_t v = 0;
    if (axp_read(AXP2101_REG_STATUS1, &v) != ESP_OK) {
        return false;
    }
    return (v & (1u << 3)) != 0;   /* STATUS1 bit 3 = battery present */
}

#else  /* !BOARD_HAS_PMIC -- no-op stubs so callers link cleanly */

esp_err_t pmic_init(void)              { return ESP_OK; }
esp_err_t pmic_rails_set(bool enabled) { (void)enabled; return ESP_OK; }
uint16_t  pmic_battery_mv(void)        { return 0; }
int       pmic_battery_pct(void)       { return -1; }
bool      pmic_battery_present(void)   { return false; }

#endif /* BOARD_HAS_PMIC */
