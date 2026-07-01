/*
 * Board: Waveshare ESP32-S3-ePaper-13.3E6
 *   - Module: ESP32-S3-WROOM-2-N32R16V (32 MB flash, 16 MB octal PSRAM)
 *   - Panel:  13.3" 1200x1600 6-color Spectra E6, dual-controller (CS_M/CS_S)
 *
 * Per-board pin map + panel geometry + palette + MCU tier + selected panel
 * driver. One of these headers is chosen at build time via boards/board.h,
 * keyed off a -DTESSERAE_BOARD_* flag in platformio.ini.
 *
 * Pinout copied verbatim from the official ESP-IDF demo:
 *   waveshareteam/ESP32-S3-ePaper-13.3E6 (epaper_port.h)
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map                                                       */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   9
#define EPD_PIN_MOSI   46
#define EPD_PIN_CS_M   10   /* drives the left  half (cols   0..599)  */
#define EPD_PIN_CS_S   3    /* drives the right half (cols 600..1199) */
#define EPD_PIN_DC     11
#define EPD_PIN_RST    2
#define EPD_PIN_BUSY   12
#define EPD_PIN_PWR    1    /* active-high panel power enable          */

#define EPD_SPI_HOST   SPI3_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. Native orientation is portrait. */
#define EPD_WIDTH      1200
#define EPD_HEIGHT     1600
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 4bpp packed = 960000 */

/* 6-color palette indices (nibble values). 4 and 7 are reserved. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3
#define EPD_COL_BLUE    0x5
#define EPD_COL_GREEN   0x6

/* ------------------------------------------------------------------ */
/* MCU tier                                                            */
/* ESP32-S3 + octal PSRAM. Framebuffers live in SPIRAM.               */
/* ------------------------------------------------------------------ */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* ------------------------------------------------------------------ */
/* Selected panel driver (see src/panel/registry.c)                    */
/* Family A: Spectra-6 SPI, dual-controller, 1200x1600.                */
/* ------------------------------------------------------------------ */
#define PANEL_DRIVER_SPECTRA6_SPI_DUAL 1
