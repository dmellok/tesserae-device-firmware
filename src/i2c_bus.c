/* i2c_bus.c -- shared I2C master bus accessor. See i2c_bus.h. */

#include "i2c_bus.h"

#include "soc/soc_caps.h"   /* SOC_I2C_NUM */

/* Cache each created bus so we call i2c_new_master_bus() at most once per port
 * per boot. Calling it a second time on an already-acquired port not only
 * fails, it runs a partial acquire/release that corrupts the live bus ("Bus not
 * freed entirely"), which then breaks the driver that DID own it. Every I2C
 * driver routes through here, so whichever runs first on a port creates that
 * bus and the rest reuse the handle. Reset to NULL on the next boot (RAM is
 * lost in deep sleep).
 *
 * One slot per hardware port: a board can run two physical buses at once (the
 * reTerminal Sticky puts its GT911 on SDA3/SCL2 and its gauge + sensors on
 * SDA1/SCL0), and a single shared handle would hand the second caller the
 * first caller's bus. */
static i2c_master_bus_handle_t s_shared_bus[SOC_I2C_NUM];

esp_err_t i2c_bus_get(int port, int sda_gpio, int scl_gpio,
                      i2c_master_bus_handle_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (port < 0 || port >= SOC_I2C_NUM) return ESP_ERR_INVALID_ARG;

    if (s_shared_bus[port] != NULL) { *out = s_shared_bus[port]; return ESP_OK; }

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
    esp_err_t err = i2c_new_master_bus(&cfg, &s_shared_bus[port]);
    if (err == ESP_OK) { *out = s_shared_bus[port]; return ESP_OK; }

    /* Port already taken, which means something that still makes its own bus
     * got there first (pmic.c, shtc3.c). Adopt it rather than failing. */
    s_shared_bus[port] = NULL;
    i2c_master_bus_handle_t existing = NULL;
    if (i2c_master_get_bus_handle(port, &existing) == ESP_OK && existing != NULL) {
        s_shared_bus[port] = existing;
        *out = existing;
        return ESP_OK;
    }
    return err;
}
