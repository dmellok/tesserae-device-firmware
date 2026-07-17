/* Sensirion SHT4x single-shot reader using the ESP-IDF 5 I2C master API. */

#include "sht4x.h"
#include "app_config.h"

#ifdef BOARD_HAS_SHT4X

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef BOARD_HAS_TOUCH
#include "i2c_bus.h"   /* shared bus: this board also carries a GT911 */
#endif

#define SHT4X_CMD_MEASURE_HIGH_PRECISION  0xFD
#define SHT4X_RESPONSE_BYTES              6
#define SHT4X_TIMEOUT_MS                  50
#define SHT4X_MEASUREMENT_DELAY_MS        10

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_sensor = NULL;

static uint8_t sht4x_crc8(const uint8_t *data, size_t len)
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

static esp_err_t sht4x_init(void)
{
    if (s_sensor != NULL) return ESP_OK;

#ifdef BOARD_HAS_TOUCH
    /* This board shares the I2C bus with the GT911 touch controller, which may
     * have created it already; get-or-create so both can coexist on port 0. */
    esp_err_t err = i2c_bus_get(BOARD_SHT4X_I2C_PORT, BOARD_SHT4X_I2C_SDA,
                                BOARD_SHT4X_I2C_SCL, &s_bus);
    if (err != ESP_OK) return err;
#else
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_SHT4X_I2C_PORT,
        .sda_io_num = BOARD_SHT4X_I2C_SDA,
        .scl_io_num = BOARD_SHT4X_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) return err;
#endif

    i2c_device_config_t device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_SHT4X_I2C_ADDR,
        .scl_speed_hz = BOARD_SHT4X_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &device_cfg, &s_sensor);
    if (err != ESP_OK) {
#ifndef BOARD_HAS_TOUCH
        /* Sole owner of this bus -> tear it down. When shared with the GT911
         * (BOARD_HAS_TOUCH) the touch driver owns the bus lifetime; leave it. */
        i2c_del_master_bus(s_bus);
#endif
        s_bus = NULL;
    }
    return err;
}

esp_err_t sht4x_read(sht4x_sample_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t err = sht4x_init();
    if (err != ESP_OK) return err;

    const uint8_t command = SHT4X_CMD_MEASURE_HIGH_PRECISION;
    err = i2c_master_transmit(s_sensor, &command, 1, SHT4X_TIMEOUT_MS);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(SHT4X_MEASUREMENT_DELAY_MS));

    uint8_t response[SHT4X_RESPONSE_BYTES];
    err = i2c_master_receive(s_sensor, response, sizeof response, SHT4X_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    if (sht4x_crc8(response, 2) != response[2] ||
        sht4x_crc8(response + 3, 2) != response[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t raw_temperature = (uint16_t)((uint16_t)response[0] << 8) | response[1];
    const uint16_t raw_humidity = (uint16_t)((uint16_t)response[3] << 8) | response[4];
    const float temperature_c = -45.0f + 175.0f * raw_temperature / 65535.0f;
    float humidity_pct = -6.0f + 125.0f * raw_humidity / 65535.0f;
    if (humidity_pct < 0.0f) humidity_pct = 0.0f;
    if (humidity_pct > 100.0f) humidity_pct = 100.0f;

    out->temperature_c = temperature_c;
    out->humidity_pct = humidity_pct;
    return ESP_OK;
}

#else

esp_err_t sht4x_read(sht4x_sample_t *out)
{
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
