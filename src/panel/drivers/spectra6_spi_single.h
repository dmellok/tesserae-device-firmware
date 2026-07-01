/*
 * Family B: Spectra-6 SPI, single-controller, 800x480.
 *
 * Concrete driver for the Seeed reTerminal E1002's 6-colour Spectra panel: one
 * chip-select, one contiguous 4bpp framebuffer (no left/right split). Ported
 * from a confirmed-working ESP-IDF path, which is
 * a direct ESP-IDF SPI path confirmed working on real E1002 hardware -- see the
 * .c for byte-level provenance. Register values are panel-specific; do not edit.
 */
#pragma once

#include "panel/epd_panel.h"

extern const epd_driver_t spectra6_spi_single_driver;
