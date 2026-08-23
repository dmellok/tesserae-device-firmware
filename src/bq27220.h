/*
 * bq27220.h -- TI BQ27220 I2C fuel gauge.
 *
 * The third way a board can report its cell, after an ADC divider and the
 * AXP2101 PMIC. Used by the Seeed reTerminal Sticky, which has no sense divider
 * to any ADC pin at all: without this the board reports no battery.
 *
 * A gauge is better than a divider, not merely different. It coulomb-counts, so
 * its state-of-charge is a measurement rather than the guess battery_pct()
 * makes from a resting voltage -- which is why bq27220_battery_pct() exists
 * separately and is preferred over running the mV through the curve.
 *
 * Nothing here needs TI's library. The three fields we want are plain 16-bit
 * little-endian standard commands, read with a write-register / repeated-start
 * / read-two-bytes transaction.
 *
 * Board configuration (see boards/seeed_reterminal_sticky.h):
 *   BOARD_BATTERY_GAUGE_I2C     enables this driver
 *   BOARD_BATTERY_GAUGE_PORT    I2C port, shared via i2c_bus_get()
 *   BOARD_BATTERY_GAUGE_SDA/SCL pins
 *   BOARD_BATTERY_GAUGE_ADDR    0x55 for a BQ27220
 *   BOARD_BATTERY_GAUGE_HZ      bus speed for this device
 */
#pragma once

#include <stdbool.h>

/* True once the gauge has returned a plausible reading, this boot or on an
 * earlier wake before deep sleep.
 *
 * Unlike the ADC and PMIC boards, whose battery_present() is a compile-time
 * board fact, a gauge can be present and still decline to answer: it NACKs
 * while it is sealed, initialising, or mid-update. Publishing 0 mV in that
 * window renders as a permanently flat cell -- the same failure the XIAO C3
 * panel's "report nothing" decision avoids -- so this stays false until a real
 * number has been seen, and battery telemetry is simply omitted meanwhile. */
bool bq27220_available(void);

/* Battery voltage in mV, or 0 if the gauge has never answered. */
int bq27220_battery_mv(void);

/* Gauge-reported state of charge, 0-100, or 0 if it has never answered. */
int bq27220_battery_pct(void);
