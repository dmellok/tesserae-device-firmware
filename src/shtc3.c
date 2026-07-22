/* Sensirion SHTC3 single-shot reader using the ESP-IDF 5 I2C master API.
 * Mirrors sht4x.c but for the SHTC3's two-command protocol.
 *
 * Unlike the SHT4x (one-byte command, own bus), the SHTC3 on the Waveshare
 * PhotoPainter 7.3" shares its I2C bus (port 0, SDA GPIO47 / SCL GPIO48) with
 * the AXP2101 PMIC. The ESP-IDF i2c-master driver allows a port to be created
 * only once -- calling i2c_new_master_bus() a second time on port 0 corrupts
 * the live bus and breaks whoever owns it. So this driver REUSES the bus the
 * PMIC created (via i2c_master_get_bus_handle), only creating its own as a
 * fallback when no bus exists yet. On the PhotoPainter the status heartbeat
 * reads the battery (-> pmic_init(), which creates the bus) before it reads the
 * environment, so the get-handle path always wins there.
 *
 * SHTC3 protocol (datasheet rev 3): 16-bit big-endian commands. A read is
 * wakeup (0x3517) -> measure T-first normal, clock-stretch disabled (0x7866)
 * -> wait for conversion -> read 6 bytes (T[2]+CRC, RH[2]+CRC) -> sleep
 * (0xB098). CRC-8 is identical to the SHT4x (poly 0x31, init 0xFF).
 */

#include "shtc3.h"
#include "app_config.h"

#ifdef BOARD_HAS_SHTC3

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_rom_sys.h"        /* esp_rom_delay_us -- guaranteed busy-wait */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SHTC3_CMD_WAKEUP              0x3517
#define SHTC3_CMD_SLEEP              0xB098
#define SHTC3_CMD_MEAS_T_NORMAL     0x7866  /* T first, normal mode, clock stretch off */
#define SHTC3_RESPONSE_BYTES         6
#define SHTC3_TIMEOUT_MS            50

/* Post-wakeup settle. The datasheet t_WU (sleep->idle) is max 240 us, after
 * which the sensor accepts a command. This MUST be a hard busy-wait, not
 * vTaskDelay: vTaskDelay(1) only blocks "until the next tick", which at
 * CONFIG_FREERTOS_HZ=1000 is anywhere from ~0 to 1 ms. When it lands short the
 * measure command races the wakeup and the sensor NAKs it. esp_rom_delay_us
 * gives a firm floor regardless of tick phase. 1000 us = ~4x the datasheet
 * max for margin. */
#define SHTC3_WAKEUP_DELAY_US     1000
#define SHTC3_MEAS_DELAY_MS         15      /* normal-mode conversion max 12.1 ms */

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_sensor = NULL;

static uint8_t shtc3_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (uint8_t)((crc & 0x80u) ? (crc << 1) ^ 0x31u : crc << 1);
        }
    }
    return crc;
}

static esp_err_t shtc3_send_cmd(uint16_t cmd)
{
    const uint8_t bytes[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    return i2c_master_transmit(s_sensor, bytes, sizeof bytes, SHTC3_TIMEOUT_MS);
}

static esp_err_t shtc3_init(void)
{
    if (s_sensor != NULL) return ESP_OK;

    /* Reuse the bus the PMIC (or anyone) already created on this port; only
     * create it ourselves if none exists. Mirrors i2c_bus.c's get-or-create so
     * we never call i2c_new_master_bus() twice on one port. */
    i2c_master_bus_handle_t existing = NULL;
    if (i2c_master_get_bus_handle(BOARD_SHTC3_I2C_PORT, &existing) == ESP_OK &&
        existing != NULL) {
        s_bus = existing;
    } else {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = BOARD_SHTC3_I2C_PORT,
            .sda_io_num = BOARD_SHTC3_I2C_SDA,
            .scl_io_num = BOARD_SHTC3_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = { .enable_internal_pullup = true },
        };
        esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
        if (err != ESP_OK) {
            s_bus = NULL;
            return err;
        }
    }

    i2c_device_config_t device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_SHTC3_I2C_ADDR,
        .scl_speed_hz = BOARD_SHTC3_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &device_cfg, &s_sensor);
    if (err != ESP_OK) {
        /* The bus belongs to the PMIC (or a fallback we made); leave it up so
         * we don't disturb a shared owner. Just drop our device handle. */
        s_sensor = NULL;
    }
    return err;
}

esp_err_t shtc3_read(shtc3_sample_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t err = shtc3_init();
    if (err != ESP_OK) return err;

    /* Wake from the low-power sleep state the previous read left it in. */
    err = shtc3_send_cmd(SHTC3_CMD_WAKEUP);
    if (err != ESP_OK) return err;
    esp_rom_delay_us(SHTC3_WAKEUP_DELAY_US);   /* hard floor; see the macro note */

    err = shtc3_send_cmd(SHTC3_CMD_MEAS_T_NORMAL);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(SHTC3_MEAS_DELAY_MS));

    uint8_t response[SHTC3_RESPONSE_BYTES];
    err = i2c_master_receive(s_sensor, response, sizeof response, SHTC3_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    if (shtc3_crc8(response, 2) != response[2] ||
        shtc3_crc8(response + 3, 2) != response[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t raw_temperature = (uint16_t)((uint16_t)response[0] << 8) | response[1];
    const uint16_t raw_humidity = (uint16_t)((uint16_t)response[3] << 8) | response[4];
    /* SHTC3 datasheet section 5.11: T = -45 + 175 * S_T / 2^16,
     *                               RH = 100 * S_RH / 2^16. */
    const float temperature_c = -45.0f + 175.0f * raw_temperature / 65536.0f;
    float humidity_pct = 100.0f * raw_humidity / 65536.0f;
    if (humidity_pct < 0.0f) humidity_pct = 0.0f;
    if (humidity_pct > 100.0f) humidity_pct = 100.0f;

    out->temperature_c = temperature_c;
    out->humidity_pct = humidity_pct;

    /* Return to low-power sleep. Best-effort: we already have the sample. */
    (void)shtc3_send_cmd(SHTC3_CMD_SLEEP);
    return ESP_OK;
}

#else

esp_err_t shtc3_read(shtc3_sample_t *out)
{
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
