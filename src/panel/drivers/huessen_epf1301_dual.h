/*
 * Spectra-6 SPI, dual-controller, 1200x1600: the Huessen EPF1301 frame.
 *
 * Same EL133UF1 glass and frame layout as the T133A01 boards, but a carrier
 * with no DC line, a display rail that has to be cycled around every paint and
 * a vendor-specific power-up / teardown, so it gets its own driver rather than
 * more variants in spectra6_t133a01_dual -- see the .c for provenance.
 */
#pragma once

#include "panel/epd_panel.h"

extern const epd_driver_t huessen_epf1301_dual_driver;
