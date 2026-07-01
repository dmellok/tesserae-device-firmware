/*
 * Family C: monochrome SPI, single-controller, 800x480 (UC8179-class).
 *
 * Concrete driver for the Seeed reTerminal E1001's 7.5" black/white panel: one
 * chip-select, a DC command/data line, and a packed 1bpp framebuffer (bit 1 =
 * white). Init/refresh ported from bb_epaper's EP75_800x480 (the panel reference
 * selects for BOARD_SEEED_RETERMINAL_E1001). Register values are panel-specific.
 */
#pragma once

#include "panel/epd_panel.h"

extern const epd_driver_t mono_spi_driver;
