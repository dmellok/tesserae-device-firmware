/*
 * m5ioe1.h -- M5Stack M5IOE1 I2C IO expander, output pins only.
 *
 * The M5Stack PaperMono hangs its panel's reset and 3.3 V enable, the touch
 * controller's reset and power, the microSD rail and the RGB LED off this
 * chip instead of MCU GPIOs, so a driver that would normally toggle a GPIO
 * asks this module instead. It is deliberately small: configure a pin as a
 * push-pull output and drive it. Inputs, ADC, PWM, interrupts and the
 * NeoPixel engine the part also has are not wrapped.
 *
 * Board contract (all five required; the module compiles to nothing without
 * BOARD_M5IOE1_I2C_PORT):
 *
 *     #define BOARD_M5IOE1_I2C_PORT   0
 *     #define BOARD_M5IOE1_I2C_SDA    <gpio>
 *     #define BOARD_M5IOE1_I2C_SCL    <gpio>
 *     #define BOARD_M5IOE1_I2C_HZ     100000
 *     #define BOARD_M5IOE1_I2C_ADDR   0x4F
 *
 * Pin numbering is the chip's 0-based index: M5's PYG1 / M5IOE1_PIN_1 is
 * index 0, PYG14 is index 13. Register map from m5stack/M5IOE1 src/M5IOE1.h
 * (16-bit registers, low byte first, bit n = pin index n).
 *
 * The bus is shared (on the PaperMono with the M5PM1 PMIC, the RTC, the IMU
 * and the touch controller), so it comes from i2c_bus_get(), never from a
 * private i2c_new_master_bus().
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Probe the expander (reads its UID). Idempotent; every other call does this
 * lazily, so boards need not call it. Returns ESP_ERR_NOT_FOUND when the chip
 * does not answer after a few attempts (it may be in its I2C idle sleep). */
esp_err_t m5ioe1_init(void);

/* True once the expander has answered on this boot. */
bool m5ioe1_available(void);

/* Configure `pin_index` (0-13) as a push-pull output the first time it is
 * touched, then drive it to `level` (0 or 1). The expander keeps its output
 * registers while the MCU deep-sleeps, so a rail left high stays high. */
esp_err_t m5ioe1_set_output(int pin_index, int level);
