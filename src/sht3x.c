/* Sensirion SHT3x (SHT30/31/35) single-shot reader using the ESP-IDF 5
 * I2C master API. Mirrors shtc3.c, minus the wake/sleep dance -- the SHT3x
 * idles on its own after a single-shot conversion.
 *
 * On the M5Stack M5Paper the SHT30 sits at 0x44 on the internal I2C bus
 * (SDA GPIO21 / SCL GPIO22), shared with the GT911 touch controller. The bus
 * is acquired through i2c_bus_get() so whichever driver runs first on the
 * port creates it and the other adopts the handle.
 *
 * SHT3x protocol (datasheet rev 6): 16-bit big-endian commands. A read is
 * single-shot, high repeatability, clock stretching disabled (0x2400) ->
 * wait for the conversion (max 15.5 ms) -> read 6 bytes (T[2]+CRC,
 * RH[2]+CRC). CRC-8 is identical to the SHT4x / SHTC3 (poly 0x31, init 0xFF).
 * Conversion: T = -45 + 175 * S_T / (2^16 - 1), RH = 100 * S_RH / (2^16 - 1).
 */

#include "sht3x.h"
#include "app_config.h"

#ifdef BOARD_HAS_SHT3X

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2c_bus.h"

#define SHT3X_CMD_MEAS_HIGH_NOSTRETCH  0x2400  /* single-shot, high rep, no clock stretch */
#define SHT3X_RESPONSE_BYTES           6
#define SHT3X_TIMEOUT_MS               50
#define SHT3X_MEAS_DELAY_MS            20      /* high-rep conversion max 15.5 ms */

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_sensor = NULL;

static uint8_t sht3x_crc8(const uint8_t *data, size_t len)
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

static esp_err_t sht3x_send_cmd(uint16_t cmd)
{
    const uint8_t bytes[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    return i2c_master_transmit(s_sensor, bytes, sizeof bytes, SHT3X_TIMEOUT_MS);
}

static esp_err_t sht3x_init(void)
{
    if (s_sensor != NULL) return ESP_OK;

    /* Shared get-or-create: the M5Paper runs the GT911 on the same port, so
     * whichever driver initialises first owns the bus and the other adopts it
     * (see i2c_bus.c -- calling i2c_new_master_bus twice on one port corrupts
     * it). Deep sleep clears the static, so this runs every wake. */
    esp_err_t err = i2c_bus_get(BOARD_SHT3X_I2C_PORT, BOARD_SHT3X_I2C_SDA,
                                BOARD_SHT3X_I2C_SCL, &s_bus);
    if (err != ESP_OK) {
        s_bus = NULL;
        return err;
    }

    i2c_device_config_t device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_SHT3X_I2C_ADDR,
        .scl_speed_hz = BOARD_SHT3X_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &device_cfg, &s_sensor);
    if (err != ESP_OK) s_sensor = NULL;
    return err;
}

esp_err_t sht3x_read(sht3x_sample_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t err = sht3x_init();
    if (err != ESP_OK) return err;

    err = sht3x_send_cmd(SHT3X_CMD_MEAS_HIGH_NOSTRETCH);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(SHT3X_MEAS_DELAY_MS));

    uint8_t response[SHT3X_RESPONSE_BYTES];
    err = i2c_master_receive(s_sensor, response, sizeof response, SHT3X_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    if (sht3x_crc8(response, 2) != response[2] ||
        sht3x_crc8(response + 3, 2) != response[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t raw_temperature = (uint16_t)((uint16_t)response[0] << 8) | response[1];
    const uint16_t raw_humidity = (uint16_t)((uint16_t)response[3] << 8) | response[4];
    const float temperature_c = -45.0f + 175.0f * raw_temperature / 65535.0f;
    float humidity_pct = 100.0f * raw_humidity / 65535.0f;
    if (humidity_pct < 0.0f) humidity_pct = 0.0f;
    if (humidity_pct > 100.0f) humidity_pct = 100.0f;

    out->temperature_c = temperature_c;
    out->humidity_pct = humidity_pct;
    return ESP_OK;
}

#else

esp_err_t sht3x_read(sht3x_sample_t *out)
{
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
