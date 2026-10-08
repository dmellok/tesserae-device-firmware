/*
 * Board: Good Display ESP32-133C02
 *   - Module: ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM)
 *   - Panel:  GDEP133C02 13.3" Spectra 6, dual-chip, 1200x1600
 *
 * The glass is the same T133A01/GDEP133C02 family supported by the ESPHome
 * epaper_spectra6_133 component. Reuse Tesserae's T133A01 dual-controller
 * driver; this header supplies the Good Display carrier pin map from the
 * philippwaller/esphome-epaper-spectra6-133 board package.
 */
#pragma once

/* Panel pin map (SPI2). DATA1/DATA2/DATA3 are reserved by the carrier; the
 * current standard-SPI driver uses DATA0 (MOSI), CLK, CS0/CS1, D/C and BUSY. */
#define EPD_PIN_SCLK   9
#define EPD_PIN_MOSI   41   /* DATA0 */
#define EPD_PIN_CS_M   18   /* CS0 -> primary/left controller */
#define EPD_PIN_CS_S   17   /* CS1 -> secondary/right controller */
#define EPD_PIN_DC     2    /* Reserved but wired; used by this firmware. */
#define EPD_PIN_RST    6
#define EPD_PIN_BUSY   7    /* active low: 0 = busy */
#define EPD_PIN_PWR    45   /* active-high panel power enable; strapping pin */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. Portrait-native 1200x1600, 4bpp packed dual-controller. */
#define EPD_WIDTH      1200
#define EPD_HEIGHT     1600
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)

/* The shared T133A01 driver takes Good Display's own init values for this
 * glass (analog timing, CDI, boosters; no DCDC) instead of the E1004's. */
#define EPD_T133_GDEP133C02_INIT 1

/* 6-colour Spectra palette indices (nibble values). */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3
#define EPD_COL_BLUE    0x5
#define EPD_COL_GREEN   0x6

#define TESSERAE_DEVICE_MODEL  "GoodDisplay_ESP32_133C02"
#define TESSERAE_DEVICE_KIND   "gooddisplay_esp32_133c02"
#define TESSERAE_BLE_HARDWARE_CODE  18

/* Relay pairing facts from Tesserae's hardware catalog entry. */
#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "spectra_6"

/* microSD: dedicated SPI bus on the Good Display carrier. */
#define TESSERAE_SD_SLOT  1
#define SD_USE_DEDICATED_SPI  1
#define SD_SPI_HOST       SPI3_HOST
#define SD_PIN_SCLK       8
#define SD_PIN_MOSI       3
#define SD_PIN_MISO       5
#define SD_PIN_CS         15
#define SD_SPI_HZ         (20 * 1000 * 1000)

/* Ref2 boards add three onboard buttons, active-high. They are harmless on
 * Ref1 where the pads are absent, so keep one firmware image for both revs. */
#define BOARD_BTN_REFRESH_PIN  12
#define BOARD_BTN_LEFT_PIN     13
#define BOARD_BTN_RIGHT_PIN    14
#define BOARD_BUTTON_ACTIVE_HIGH 1

#define MCU_TIER_S3_OCTAL_PSRAM 1
#define PANEL_DRIVER_SPECTRA6_T133A01_DUAL 1
