/*
 * max17048.h -- Analog Devices (Maxim) MAX17048 I2C fuel gauge, OPTIONAL.
 *
 * A fourth way a board can report its cell, for hardware that has no sense of
 * its own and gets a gauge added by the owner. The XIAO 7.5" ePaper panel with
 * the ESP32-C3 has no divider to any ADC pin (every analog pin drives the
 * display), so an Adafruit MAX17048 breakout (product 5580) on D4/D5 is the
 * only way it can see its battery (issue #61).
 *
 * Unlike the BQ27220 on the reTerminal Sticky, the gauge here may simply not
 * be fitted. So the first read of each wake probes for it (an address probe,
 * then the VERSION register, which reads 0x001x on this part). A board with no
 * gauge NACKs the probe, battery_present() stays false, and battery telemetry
 * is omitted exactly as it was before this driver existed.
 *
 * The registers we use are plain 16-bit big-endian reads:
 *   0x02 VCELL    78.125 uV per LSB
 *   0x04 SOC      high byte = percent, low byte = 1/256 percent
 *   0x08 VERSION  0x001x
 *
 * Board configuration (see boards/seeed_xiao_epaper_75_c3.h):
 *   BOARD_BATTERY_MAX17048      enables this driver
 *   BOARD_MAX17048_PORT         I2C port, shared via i2c_bus_get()
 *   BOARD_MAX17048_SDA/SCL      pins
 *   BOARD_MAX17048_ADDR         0x36
 *   BOARD_MAX17048_HZ           bus speed for this device
 */
#pragma once

#include <stdbool.h>

/* True once the gauge has answered with a plausible reading this wake, or on
 * a recent earlier wake (a single missed read keeps the last sample; repeated
 * misses clear it, so a gauge that is unplugged stops being reported). False
 * on a board without a gauge. */
bool max17048_available(void);

/* Cell voltage in mV, or 0 if no gauge has answered. */
int max17048_battery_mv(void);

/* Gauge state of charge, clamped to 0-100, or 0 if no gauge has answered. */
int max17048_battery_pct(void);
