/*
 * Board: Seeed reTerminal E1002
 *   - MCU:   XIAO ESP32-S3 class (ESP32-S3 + PSRAM)
 *   - Panel: 6-colour Spectra-6, single-controller, 800x480
 *
 * Family B. Uses the spectra6_spi_single driver, ported from a confirmed-working ESP-IDF path's
 * confirmed-working ESP-IDF path. Single chip-select (no CS_M/CS_S split) and
 * no software power-enable pin. Pin map from bb_epaper DEV_Config.h
 * (BOARD_SEEED_RETERMINAL_E1002) and the the reference spectra6 init.
 *
 * VERIFY the server-side renderer: the frame must be packed as 800x480 4bpp
 * Spectra-6 (192000 bytes) for TESSERAE_DEVICE_KIND below.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (XIAO ESP32-S3 GPIO numbers)                          */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   7
#define EPD_PIN_MOSI   9
#define EPD_PIN_CS     10   /* single controller chip-select */
#define EPD_PIN_DC     11
#define EPD_PIN_RST    12
#define EPD_PIN_BUSY   13   /* active low: 0 = busy */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (20 * 1000 * 1000)   /* the reference uses 20 MHz */

/* Panel geometry. Landscape-native 800x480, 4bpp packed = 1 controller. */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 4bpp packed = 192000 */

/* 6-color Spectra palette indices (nibble values). White = 0x1 matches the
 * reference's 0x11 clear byte. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3
#define EPD_COL_BLUE    0x5
#define EPD_COL_GREEN   0x6

/* Board model -> default device id "reTerminal_E1002_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "reTerminal_E1002"

/* Tesserae hardware-catalog kind. Uses the *native* 4bpp bin manifest
 * (esp32_client protocol + esp32_bin renderer -> 800x480 4bpp Spectra-6,
 * 192000 bytes). The plain "seeed_reterminal_e1002" id is reserved for
 * legacy-flashed E1002s, so the unified firmware uses the _native id. */
#define TESSERAE_DEVICE_KIND   "seeed_reterminal_e1002_native"

/* MCU tier: ESP32-S3 + PSRAM (assumed octal; verify on hardware). */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Selected panel driver: Family B, single-controller Spectra-6. */
#define PANEL_DRIVER_SPECTRA6_SPI_SINGLE 1
