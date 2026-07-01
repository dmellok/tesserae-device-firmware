/*
 * Waveshare 13.3" Spectra E6 (6-color) e-paper driver.
 *
 * Ported from waveshareteam/ESP32-S3-ePaper-13.3E6 (epaper_port.c).
 * The init byte sequence and command set are panel-specific and must
 * stay byte-for-byte exact -- don't "clean them up."
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "app_config.h"

/* One-time SPI bus + GPIO setup. Safe to call multiple times. */
esp_err_t epd_port_init(void);

/* Full panel power-up + reset + init sequence. Call after epd_port_init()
 * and before epd_clear()/epd_display(). */
void epd_init(void);

/* Fill the entire panel with a single palette color. */
void epd_clear(uint8_t color);

/* Push a full-frame buffer. `image` must point to EPD_BUF_BYTES bytes
 * (960000) laid out as 1600 rows of 600 packed-nibble bytes -- two
 * pixels per byte, high nibble = even column. The driver routes the
 * first 300 bytes of each row to the left controller (CS_M) and the
 * next 300 to the right (CS_S), so callers just produce a single
 * contiguous buffer. */
void epd_display(const uint8_t *image);

/* Paint the 6 panel colours as horizontal bands, top to bottom in palette
 * order (black, white, yellow, red, blue, green). The user-facing splash
 * -- if every band shows the expected ink, the panel + driver + LUT are
 * all healthy. Note: on a brand-new panel the very first refresh can show
 * ghosting from the shipping state; press RESET to refresh again. */
void epd_show_color_bars(void);

/* Diagnostic: paint all 8 possible nibble values (0x0..0x7) as 8 bands.
 * Useful when colours look wrong -- the output tells you the true
 * nibble->colour LUT of a specific panel batch, which can drift from the
 * vendor reference. Not called in production; wire it in temporarily if
 * epd_show_color_bars renders unexpected colours. */
void epd_show_palette_sweep(void);

/* Send DEEP_SLEEP command and drop the panel power rail. After this,
 * epd_init() must be called again before the next refresh. */
void epd_sleep(void);
