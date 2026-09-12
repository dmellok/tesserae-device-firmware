/* m5ioe1.c -- M5Stack M5IOE1 I2C IO expander, output pins only. See m5ioe1.h. */

#include "m5ioe1.h"
#include "app_config.h"   /* pulls board.h -> BOARD_M5IOE1_* */

#ifdef BOARD_M5IOE1_I2C_PORT

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

/* Register map (m5stack/M5IOE1 src/M5IOE1.h). Every GPIO register is a
 * 16-bit pair, _L holding pins 1-8 (indices 0-7) and _H pins 9-14; a write of
 * [reg, lo, hi] lands on both, exactly as the vendor library does it. */
#define REG_UID_L         0x00
#define REG_GPIO_MODE_L   0x03   /* 1 = output */
#define REG_GPIO_OUT_L    0x05   /* 1 = high */
#define REG_GPIO_PU_L     0x09   /* 1 = pull-up on */
#define REG_GPIO_PD_L     0x0B   /* 1 = pull-down on */
#define REG_GPIO_DRV_L    0x13   /* 1 = open-drain, 0 = push-pull */
#define REG_I2C_CFG       0x23   /* [3:0] idle-sleep timeout s, 0 = never */

#define IOE_TIMEOUT_MS    50
#define IOE_PROBE_TRIES   3
#define IOE_PIN_COUNT     14

static const char *TAG = "m5ioe1";

static i2c_master_dev_handle_t s_dev;
static bool     s_ready;
static uint16_t s_configured;   /* pins already set up as push-pull outputs */

static bool rd16(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, rx, sizeof rx, IOE_TIMEOUT_MS) != ESP_OK)
        return false;
    *out = (uint16_t)rx[0] | ((uint16_t)rx[1] << 8);
    return true;
}

static bool wr16(uint8_t reg, uint16_t v)
{
    uint8_t tx[3] = { reg, (uint8_t)(v & 0xff), (uint8_t)(v >> 8) };
    return i2c_master_transmit(s_dev, tx, sizeof tx, IOE_TIMEOUT_MS) == ESP_OK;
}

static bool rd8(uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, 1, IOE_TIMEOUT_MS) == ESP_OK;
}

static bool wr8(uint8_t reg, uint8_t v)
{
    uint8_t tx[2] = { reg, v };
    return i2c_master_transmit(s_dev, tx, sizeof tx, IOE_TIMEOUT_MS) == ESP_OK;
}

/* Clear bit `pin` in the register at `reg` (read-modify-write). */
static bool clear_bit(uint8_t reg, int pin)
{
    uint16_t v;
    if (!rd16(reg, &v)) return false;
    if (!(v & (1u << pin))) return true;
    return wr16(reg, v & ~(1u << pin));
}

esp_err_t m5ioe1_init(void)
{
    if (s_ready) return ESP_OK;

    if (s_dev == NULL) {
        i2c_master_bus_handle_t bus = NULL;
        esp_err_t err = i2c_bus_get(BOARD_M5IOE1_I2C_PORT, BOARD_M5IOE1_I2C_SDA,
                                    BOARD_M5IOE1_I2C_SCL, &bus);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2C bus unavailable: %s", esp_err_to_name(err));
            return err;
        }
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = BOARD_M5IOE1_I2C_ADDR,
            .scl_speed_hz    = BOARD_M5IOE1_I2C_HZ,
        };
        err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
        if (err != ESP_OK) {
            s_dev = NULL;
            return err;
        }
    }

    /* The part can be in its I2C idle sleep and NACK the first transaction;
     * the vendor library retries with a 50 ms gap for the same reason. */
    uint16_t uid = 0;
    for (int attempt = 1; attempt <= IOE_PROBE_TRIES; attempt++) {
        if (rd16(REG_UID_L, &uid)) {
            s_ready = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_ready) {
        ESP_LOGW(TAG, "no answer at 0x%02x after %d tries",
                 BOARD_M5IOE1_I2C_ADDR, IOE_PROBE_TRIES);
        return ESP_ERR_NOT_FOUND;
    }

    /* Keep it awake: a chip that dozes between our sparse writes costs a
     * retry on every rail toggle. Only the timeout nibble changes; speed and
     * the INT pull bits stay whatever the board shipped with. */
    uint8_t i2c_cfg = 0;
    if (rd8(REG_I2C_CFG, &i2c_cfg) && (i2c_cfg & 0x0f)) {
        (void)wr8(REG_I2C_CFG, (uint8_t)(i2c_cfg & ~0x0f));
    }

    ESP_LOGI(TAG, "IO expander uid=0x%04x at 0x%02x", uid, BOARD_M5IOE1_I2C_ADDR);
    return ESP_OK;
}

bool m5ioe1_available(void)
{
    return s_ready;
}

esp_err_t m5ioe1_set_output(int pin_index, int level)
{
    if (pin_index < 0 || pin_index >= IOE_PIN_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t err = m5ioe1_init();
    if (err != ESP_OK) return err;

    const uint16_t bit = (uint16_t)(1u << pin_index);

    /* First touch on this boot: push-pull output, no pulls. The vendor
     * library's pinMode(OUTPUT) writes the same four registers in the same
     * order (PU, PD, DRV, then MODE last so the pin never drives with a
     * stale drive-mode). Static state resets on every wake while the chip
     * keeps its registers, so this is a cheap re-assert, not a glitch. */
    if (!(s_configured & bit)) {
        uint16_t mode;
        bool ok = clear_bit(REG_GPIO_PU_L, pin_index) &&
                  clear_bit(REG_GPIO_PD_L, pin_index) &&
                  clear_bit(REG_GPIO_DRV_L, pin_index) &&
                  rd16(REG_GPIO_MODE_L, &mode);
        if (ok && !(mode & bit)) {
            /* Preload the level before the pin becomes an output, so a rail
             * that should come up high never dips low first. */
            uint16_t out;
            if (rd16(REG_GPIO_OUT_L, &out)) {
                out = level ? (out | bit) : (out & ~bit);
                (void)wr16(REG_GPIO_OUT_L, out);
            }
            ok = wr16(REG_GPIO_MODE_L, mode | bit);
        }
        if (!ok) {
            ESP_LOGW(TAG, "pin %d: output setup failed", pin_index);
            return ESP_FAIL;
        }
        s_configured |= bit;
    }

    uint16_t out;
    if (!rd16(REG_GPIO_OUT_L, &out)) return ESP_FAIL;
    uint16_t want = level ? (out | bit) : (out & ~bit);
    if (want == out) return ESP_OK;
    if (!wr16(REG_GPIO_OUT_L, want)) {
        ESP_LOGW(TAG, "pin %d: write %d failed", pin_index, level);
        return ESP_FAIL;
    }
    return ESP_OK;
}

#endif /* BOARD_M5IOE1_I2C_PORT */
