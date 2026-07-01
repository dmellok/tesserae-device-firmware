/*
 * Board: Seeed Studio XIAO ePaper Driver Board EE02
 *   - MCU:   XIAO ESP32-S3 Plus (ESP32-S3 + PSRAM), native USB (no CH340)
 *   - Panel: 13.3" T133A01, dual-chip Spectra-6, 1200x1600 (six-colour E6)
 *
 * The panel is the SAME T133A01 as the reTerminal E1004, so this board REUSES
 * the spectra6_t133a01_dual driver unchanged (byte-identical init/refresh). Only
 * the pin map differs -- taken from Seeed's own Seeed_GFX
 * (User_Setups/EPaper_Board_Pins_Setups.h, USE_XIAO_EPAPER_DISPLAY_BOARD_EE02):
 *   TFT_CS=44 (CS_M), TFT_CS1=41 (CS_S), TFT_DC=10, TFT_BUSY=4, TFT_RST=38,
 *   TFT_ENABLE=43, SCLK=D8=GPIO7, MOSI=D10=GPIO9.
 *
 * NOTE: CS_M (44) and EN (43) are the ESP32-S3 UART0 TX/RX pins. This env's
 * sdkconfig (sdkconfig.ee02.defaults, wired via platformio.ini) moves the
 * console to USB-Serial-JTAG so UART0 doesn't fight the panel.
 *
 * Frame is the same 960000-byte 1200x1600 4bpp Spectra layout as the E1004, so
 * the server reuses that renderer under TESSERAE_DEVICE_KIND below.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (dual controller, SPI2) -- Seeed EE02 GPIO numbers     */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   7    /* XIAO D8  */
#define EPD_PIN_MOSI   9    /* XIAO D10 */
#define EPD_PIN_CS_M   44   /* TFT_CS  -> primary controller  (left half)  */
#define EPD_PIN_CS_S   41   /* TFT_CS1 -> second controller   (right half) */
#define EPD_PIN_DC     10
#define EPD_PIN_RST    38
#define EPD_PIN_BUSY   4    /* active low: 0 = busy */
#define EPD_PIN_PWR    43   /* TFT_ENABLE: active-high panel power enable   */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. Portrait-native 1200x1600, 4bpp packed dual-controller. */
#define EPD_WIDTH      1200
#define EPD_HEIGHT     1600
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 4bpp packed = 960000 */

/* 6-colour Spectra palette indices (nibble values). Matches the E1004. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3
#define EPD_COL_BLUE    0x5
#define EPD_COL_GREEN   0x6

/* Board model -> default device id "Seeed_EE02_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "Seeed_EE02"

/* Tesserae hardware-catalog kind. Same 1200x1600 4bpp Spectra-6 frame (960000
 * bytes) as the E1004, so the server maps this to the existing renderer. */
#define TESSERAE_DEVICE_KIND   "seeed_ee02"

/* No battery-sense pin wired yet (Seeed_GFX exposes none; the board has only a
 * charge-status LED). battery_read_mv() -> 0 = "unknown" until we find the ADC
 * pin from the schematic. Intentionally no BOARD_BATTERY_ADC_CHANNEL. */

/* MCU tier: ESP32-S3 + octal PSRAM. */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Selected panel driver: Family A variant, T133A01 dual-chip (shared w/ E1004). */
#define PANEL_DRIVER_SPECTRA6_T133A01_DUAL 1
