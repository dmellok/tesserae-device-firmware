/*
 * Family A: Spectra-6 SPI, dual-controller, 1200x1600.
 *
 * Concrete driver for the Waveshare 13.3" Spectra E6 (6-color) glass and
 * its GDEP133C02-class siblings (reTerminal E1004, XIAO EE02 13.3"), which
 * split the panel into left/right halves each driven by its own controller
 * (CS_M / CS_S). The init byte sequence is panel-specific and must stay
 * byte-for-byte exact -- see the .c.
 */
#pragma once

#include "panel/epd_panel.h"

extern const epd_driver_t spectra6_spi_dual_driver;
