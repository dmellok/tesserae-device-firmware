/*
 * i2c_bus.h -- shared I2C master bus accessor.
 *
 * The ESP-IDF i2c-master driver allows a port to be created only once, so every
 * driver that wants a bus must go through this get-or-create helper rather than
 * calling i2c_new_master_bus() itself. Two boards already need it:
 *
 *   reTerminal E1003   SHT4x + GT911 touch   (port 0, SDA 19 / SCL 20)
 *   reTerminal Sticky  SHT4x + BQ27220 gauge (port 0, SDA  1 / SCL  0)
 *
 * and any board that gains a second I2C part joins them. Getting this wrong is
 * not a clean failure: a second i2c_new_master_bus() on a live port runs a
 * partial acquire/release that corrupts the bus for the driver that DID own it,
 * so the symptom lands on the innocent device.
 *
 * Compiled unconditionally. It is one static and one function, and the linker
 * drops it on boards that never call it -- cheaper than the per-board #ifdef
 * this used to carry, which is precisely the thing that would be forgotten.
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/* Get the master bus for `port`, creating it (SDA/SCL, internal pull-ups,
 * default clock) if it does not exist yet. Safe to call from any driver in any
 * order. Per-device clock speed is set when adding a device, not here, so
 * devices sharing a bus may run at different rates. */
esp_err_t i2c_bus_get(int port, int sda_gpio, int scl_gpio,
                      i2c_master_bus_handle_t *out);
