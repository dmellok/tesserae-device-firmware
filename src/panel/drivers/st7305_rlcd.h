/*
 * Family F: Sitronix ST7305 reflective LCD (RLCD) over SPI, 1bpp.
 *
 * The first non-e-paper panel in this tree. A reflective TFT with the
 * controller's own frame memory: it holds the image for as long as it is
 * powered and refreshing (the controller's low-power mode), and blanks the
 * moment power or the refresh clock goes. So where the e-paper drivers cut the
 * panel rail at sleep(), this one parks the controller in low-power mode and
 * HOLDS its SPI pins through deep sleep, and skips the hardware reset on the
 * next wake so the image never visibly drops between frames.
 *
 * First board: Waveshare ESP32-S3-RLCD-4.2 (400x300). Init sequence ported
 * from waveshareteam/ESP32-S3-RLCD-4.2 (display_bsp.cpp); the controller's
 * pixel packing (2 columns x 4 rows per byte, columns running down the panel)
 * from the same source's landscape lookup table.
 */
#pragma once

#include "panel/epd_panel.h"

extern const epd_driver_t st7305_rlcd_driver;
