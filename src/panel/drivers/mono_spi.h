/*
 * Family C: monochrome SPI, single-controller, 800x480 (UC8179-class).
 *
 * Concrete driver for the Seeed reTerminal E1001's 7.5" black/white panel: one
 * chip-select, a DC command/data line, and a packed 1bpp framebuffer (bit 1 =
 * white). Init/refresh ported from bb_epaper's EP75_800x480 (the panel used for
 * BOARD_SEEED_RETERMINAL_E1001). Register values are panel-specific.
 */
#pragma once

#include "panel/epd_panel.h"

extern const epd_driver_t mono_spi_driver;

/* Full-frame refresh mode for 1bpp frames (mono builds only; the 4-gray and
 * BWR builds ignore it and stay on their single waveform).
 *
 *   FULL  the shipped flashing refresh (bb_epaper epd75_init_sequence_full).
 *   FAST  no-flash refresh, ~1.5 s: bb_epaper's epd75_init_fast_gen2 on glass
 *         with the built-in OTP tables, its register-LUT
 *         epd75_init_sequence_fast on legacy glass (which of the two is
 *         decided by the same OTP probe the 4-gray build uses, cached in NVS).
 *
 * Default is FULL, so nothing changes unless a caller asks. Takes effect on
 * the next display(): the driver re-runs the matching init itself if the
 * panel was set up for a different mode. */
#define MONO_SPI_REFRESH_FULL 0
#define MONO_SPI_REFRESH_FAST 1
void mono_spi_set_refresh_mode(int mode);
int  mono_spi_get_refresh_mode(void);
