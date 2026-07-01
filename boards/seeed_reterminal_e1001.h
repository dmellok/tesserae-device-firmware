/*
 * Board: Seeed reTerminal E1001
 *   - MCU:   XIAO ESP32-S3 class (ESP32-S3 + PSRAM)
 *   - Panel: 7.5" mono (black/white), 800x480, UC8179-class (EP75)
 *
 * Family C. Uses the mono_spi driver. Same XIAO pins as the E1002, but the
 * panel is 1-bit monochrome, so EPD_BUF_BYTES is W*H/8 (48000) and the frame
 * is a packed 1bpp bitmap (bit 1 = white, bit 0 = black), not 4bpp Spectra.
 *
 * Panel = bb_epaper EP75_800x480 (the default for BOARD_SEEED_RETERMINAL
 * _E1001); init/refresh ported into mono_spi.c. Pins from the reTerminal pin map.
 *
 * VERIFY the server-side renderer: the frame must be 800x480 1bpp (48000 bytes)
 * for TESSERAE_DEVICE_KIND below -- Tesserae needs a mono renderer for it.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (XIAO ESP32-S3 GPIO numbers) -- same as the E1002.    */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   7
#define EPD_PIN_MOSI   9
#define EPD_PIN_CS     10
#define EPD_PIN_DC     11
#define EPD_PIN_RST    12
#define EPD_PIN_BUSY   13   /* active low: 0 = busy */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. Landscape 800x480, 1bpp packed = 8 px/byte. */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 8)   /* 1bpp packed = 48000 */

/* Monochrome. The driver treats the frame as a 1bpp bitmap; these are the
 * logical colours the shared splash uses (black on white). bit 1 = white. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1

/* Board model -> default device id "reTerminal_E1001_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "reTerminal_E1001"

/* Tesserae hardware-catalog kind. NOTE: the E1001 needs a MONO (1bpp 800x480)
 * server renderer, distinct from the Spectra esp32_bin path. Confirm this id
 * and that a matching manifest exists in your Tesserae. */
#define TESSERAE_DEVICE_KIND   "seeed_reterminal_e1001"

/* MCU tier: ESP32-S3 + PSRAM (assumed octal; verify on hardware). */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Selected panel driver: Family C, single-controller mono SPI. */
#define PANEL_DRIVER_MONO_SPI 1
