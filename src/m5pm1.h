/*
 * m5pm1.h -- M5Stack M5PM1 power-management IC: battery voltage only.
 *
 * The PaperMono's cell is not on any ESP32 ADC pin. The M5PM1 measures VBAT
 * itself and publishes it in millivolts over I2C, so this is the board's
 * battery backend (battery.c, BOARD_BATTERY_M5PM1). Power hold, shutdown,
 * the frontlight PWM, the watchdog and the wake sources the chip also owns
 * are not wrapped: the PaperMono powers up and stays up without any of them
 * being touched (M5Unified never writes them either).
 *
 * Board contract (the module compiles to nothing without BOARD_BATTERY_M5PM1):
 *
 *     #define BOARD_BATTERY_M5PM1    1
 *     #define BOARD_M5PM1_I2C_PORT   0
 *     #define BOARD_M5PM1_I2C_SDA    <gpio>
 *     #define BOARD_M5PM1_I2C_SCL    <gpio>
 *     #define BOARD_M5PM1_I2C_HZ     100000
 *     #define BOARD_M5PM1_I2C_ADDR   0x6E
 *
 * Two more of the chip's outputs are wrapped for the PaperMono, each behind
 * its own board macro (all three share BOARD_M5PM1_I2C_*):
 *   BOARD_FRONTLIGHT_M5PM1   the panel frontlight, PWM0 on the PMIC's GPIO3
 *   BOARD_LED_M5PM1          the red channel of the RGB LED, on LED_EN
 *
 * Register map from m5stack/M5PM1 src/M5PM1.h. Shares the bus through
 * i2c_bus_get() like every other I2C part.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* True once the PMIC has answered a VBAT read on this boot (or a retained
 * reading from an earlier wake exists). Mirrors bq27220_available(): a chip
 * that did not answer is "no reading", never "flat". */
bool m5pm1_battery_available(void);

/* Battery voltage in mV, or 0 when nothing is known. Read once per wake,
 * falling back to the last good sample retained across deep sleep. */
int m5pm1_battery_mv(void);

/* Frontlight brightness, 0-100 percent (0 = PWM off). Routes GPIO3 to PWM0
 * at 5 kHz the way M5's PowerDemo does; the PMIC keeps the PWM running on
 * its own, so the light survives the MCU's deep sleep until set to 0. */
esp_err_t m5pm1_frontlight_set(int pct);

/* Red status LED (LED_EN): on or off. */
esp_err_t m5pm1_led_set(bool on);
