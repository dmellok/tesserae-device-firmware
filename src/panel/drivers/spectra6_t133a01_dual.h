/*
 * Family A variant: Spectra-6 SPI, dual-controller, 1200x1600, T133A01 silicon.
 *
 * Concrete driver for the Seeed reTerminal E1004's 13.3" T133A01 panel. Shares
 * the base 13.3E6's overall shape (two controllers, left/right split buffer,
 * 4bpp Spectra-6 data) but with a T133A01-specific init sequence, refresh
 * command, and per-frame CCSET -- see the .c for byte-level provenance. Kept
 * separate from spectra6_spi_dual so the byte-for-byte 13.3E6 driver stays
 * frozen.
 */
#pragma once

#include "panel/epd_panel.h"

extern const epd_driver_t spectra6_t133a01_dual_driver;
