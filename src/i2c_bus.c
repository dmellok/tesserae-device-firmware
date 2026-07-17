/* i2c_bus.c -- shared I2C master bus accessor. See i2c_bus.h. */

#include "i2c_bus.h"

#ifdef BOARD_HAS_TOUCH

esp_err_t i2c_bus_get(int port, int sda_gpio, int scl_gpio,
                      i2c_master_bus_handle_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    i2c_master_bus_config_t cfg = {
        .i2c_port = port,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    esp_err_t err = i2c_new_master_bus(&cfg, out);
    if (err == ESP_ERR_INVALID_STATE) {
        /* The other driver on this bus already created it -- reuse its handle. */
        err = i2c_master_get_bus_handle(port, out);
    }
    return err;
}

#endif /* BOARD_HAS_TOUCH */
