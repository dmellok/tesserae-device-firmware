/*
 * i2c_bus.h -- shared I2C master bus accessor.
 *
 * The reTerminal E1003 carries two I2C devices on one physical bus (port 0,
 * SDA GPIO19 / SCL GPIO20): the SHT4x and the GT911 touch controller. The
 * ESP-IDF i2c-master driver allows a port to be created only once, so both
 * drivers must share a single bus handle rather than each calling
 * i2c_new_master_bus(). This get-or-create helper returns the existing bus for
 * a port if one was already made, else creates it.
 *
 * Compiled only where a shared bus is actually needed (BOARD_HAS_TOUCH); the
 * boards that have just the SHT4x keep creating their own bus in sht4x.c and
 * are unaffected.
 */
#pragma once

#include "app_config.h"

#ifdef BOARD_HAS_TOUCH

#include "driver/i2c_master.h"
#include "esp_err.h"

/* Get the master bus for `port`, creating it (SDA/SCL, internal pull-ups,
 * default clock) if it does not exist yet. Safe to call from either driver in
 * any order. */
esp_err_t i2c_bus_get(int port, int sda_gpio, int scl_gpio,
                      i2c_master_bus_handle_t *out);

#endif /* BOARD_HAS_TOUCH */
