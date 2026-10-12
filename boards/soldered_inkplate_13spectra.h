/*
 * Board: Soldered Inkplate 13SPECTRA
 *   - Module: ESP32-S3-WROOM-2 (octal PSRAM)
 *   - Panel:  E Ink EL133UF1 13.3" Spectra 6, dual-chip, 1200x1600
 *
 * Same glass family as the Seeed E1004 / Good Display 133C02, so this reuses
 * the T133A01 dual-controller driver with Soldered's init values
 * (EPD_T133_INKPLATE13_INIT) and the pin map below. Pins and init bytes come
 * from Soldered's Inkplate-Arduino-library (Inkplate13SPECTRA/pins.h) and the
 * Soldered-Inkplate-ESPHome inkplate_spi component.
 *
 * On hardware: boots, paints the setup QR and a full-colour frame. Soldered
 * holds DC high under the BS0=0/BS1=1 strap; toggling it per command, as this
 * driver does, works too. Battery, the wake button and deep sleep are not yet
 * checked on the board.
 */
#pragma once

/* Panel pin map (SPI2). */
#define EPD_PIN_SCLK   38
#define EPD_PIN_MOSI   40
#define EPD_PIN_CS_M   42   /* primary/left controller  */
#define EPD_PIN_CS_S   39   /* secondary/right controller */
#define EPD_PIN_DC     14
#define EPD_PIN_RST    4
#define EPD_PIN_BUSY   7    /* active low: 0 = busy */
#define EPD_PIN_PWR    21   /* active-high panel power enable */
#define EPD_PIN_BS0    6    /* interface-select strap, driven LOW  */
#define EPD_PIN_BS1    5    /* interface-select strap, driven HIGH */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. Portrait-native 1200x1600, 4bpp packed dual-controller. */
#define EPD_WIDTH      1200
#define EPD_HEIGHT     1600
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)

/* The server's catalog entry for this kind is landscape 1600x1200, for the
 * community inkplate-tesserae-driver firmware, which turns frames on the
 * device. This firmware paints the portrait buffer as-is, so it registers with
 * rotation 0 to declare 1200x1600 as its framebuffer; the server then packs
 * frames at that stride and turns a landscape dashboard onto it. */
#define BOARD_FRAMEBUFFER_ROTATION 0

/* Soldered's init values and slower power/reset timing. */
#define EPD_T133_INKPLATE13_INIT 1
#define EPD_T133_RST_MS          100

/* 6-colour Spectra palette indices (nibble values). */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3
#define EPD_COL_BLUE    0x5
#define EPD_COL_GREEN   0x6

#define TESSERAE_DEVICE_MODEL  "Soldered_Inkplate_13SPECTRA"
#define TESSERAE_DEVICE_KIND   "soldered_inkplate_13spectra"
#define TESSERAE_BLE_HARDWARE_CODE  19

#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "spectra_6"

/* Wake button: GPIO18, active-low (ext1 ANY_LOW path in buttons.h). */
#define BOARD_BTN_REFRESH_PIN  18

/* Battery: ADC GPIO1 (ADC1 ch0), x2 divider, enabled via PCAL6416A P1-1 (expander
 * pin 9; Soldered readBattery()). Only reads with jumper JP5 shorted -- open, the
 * divider is cut off and the ADC sees ~0 (reported as "unknown", not flat).
 * I2C is SDA 8 / SCL 9: Soldered's library calls Wire.begin() with no pins and
 * builds on the generic esp32-s3-devkitc-1 variant (inkplate-tesserae-driver
 * firmware/platformio.ini), whose default Wire pins are 8/9. */

#define BOARD_BATTERY_ADC_CHANNEL        ADC_CHANNEL_0
#define BOARD_BATTERY_DIVIDER            2
#define BOARD_VBAT_SWITCH_EXP_PIN        1      /* port-1 bit; expander pin 9 */
#define BOARD_VBAT_SWITCH_EXP_ADDR       0x20
#define BOARD_VBAT_SWITCH_EXP_I2C_PORT   0
#define BOARD_VBAT_SWITCH_EXP_I2C_SDA    8
#define BOARD_VBAT_SWITCH_EXP_I2C_SCL    9

#define MCU_TIER_S3_OCTAL_PSRAM 1
#define PANEL_DRIVER_SPECTRA6_T133A01_DUAL 1
