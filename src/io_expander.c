/* io_expander.c -- see io_expander.h. */

#include "io_expander.h"
#include "i2c_bus.h"

esp_err_t io_exp_open(io_exp_t *x, int port, int sda, int scl, uint8_t addr, uint32_t hz)
{
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_bus_get(port, sda, scl, &bus);
    if (err != ESP_OK) return err;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = hz,
    };
    return i2c_master_bus_add_device(bus, &dc, &x->dev);
}

bool io_exp_write(const io_exp_t *x, uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = { reg, val };
    return x->dev && i2c_master_transmit(x->dev, b, sizeof b, 50) == ESP_OK;
}

bool io_exp_read(const io_exp_t *x, uint8_t reg, uint8_t *val)
{
    return x->dev && i2c_master_transmit_receive(x->dev, &reg, 1, val, 1, 50) == ESP_OK;
}
