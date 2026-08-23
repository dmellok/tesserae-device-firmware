/* i2c_bus.c -- shared I2C master bus accessor. See i2c_bus.h. */

#include "i2c_bus.h"

/* Cache the created bus so we call i2c_new_master_bus() at most once per boot.
 * Calling it a second time on an already-acquired port not only fails, it runs
 * a partial acquire/release that corrupts the live bus ("Bus not freed
 * entirely"), which then breaks the driver that DID own it. Every I2C driver
 * routes through here, so whichever runs first creates the bus and the rest
 * reuse this handle. Reset to NULL on the next boot (RAM is lost in deep
 * sleep). */
static i2c_master_bus_handle_t s_shared_bus = NULL;

esp_err_t i2c_bus_get(int port, int sda_gpio, int scl_gpio,
                      i2c_master_bus_handle_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    if (s_shared_bus != NULL) { *out = s_shared_bus; return ESP_OK; }

    /* Create first, adopt second. The other order reads more defensively but
     * asks i2c_master_get_bus_handle() about a port that usually does not exist
     * yet, and that logs an ERROR line -- on the common path, every boot, on
     * every board with an I2C part. A log that cries wolf on a healthy boot is
     * worse than no log. */
    i2c_master_bus_config_t cfg = {
        .i2c_port = port,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_shared_bus);
    if (err == ESP_OK) { *out = s_shared_bus; return ESP_OK; }

    /* Port already taken, which means something that still makes its own bus
     * got there first (pmic.c, shtc3.c). Adopt it rather than failing. */
    s_shared_bus = NULL;
    i2c_master_bus_handle_t existing = NULL;
    if (i2c_master_get_bus_handle(port, &existing) == ESP_OK && existing != NULL) {
        s_shared_bus = existing;
        *out = existing;
        return ESP_OK;
    }
    return err;
}
