/*
 * Board: Waveshare ESP32-S3-Zero + 10.85-inch e-Paper G
 *   - Module: ESP32-S3FH4R2 (4 MB flash, 2 MB embedded Quad PSRAM)
 *   - Panel:  1360x480 B/W/Y/R, dual 680-pixel controller, 2bpp indexed frame
 *
 * Verified no-solder pinout for the ESP32-S3 Zero. It deliberately differs
 * from Waveshare's ESP32-S3 reference demo (DC=13, RST=14, BUSY=4), whose
 * pins require soldering to pads on this carrier.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map                                                      */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   12
#define EPD_PIN_MOSI   11
#define EPD_PIN_CS_M   10   /* drives columns   0..679  */
#define EPD_PIN_CS_S    9   /* drives columns 680..1359 */
#define EPD_PIN_DC      8
#define EPD_PIN_RST     7
#define EPD_PIN_BUSY    6
#define EPD_PIN_PWR     5

/* Panel geometry. Native orientation is landscape. */
#define EPD_WIDTH      1360
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 4)   /* 2bpp packed = 163200 */

/* Four-colour palette indices. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3

/* Board model -> default device id "Waveshare_1085G_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "Waveshare_1085G"

/* Tesserae hardware-catalog kind: native 2bpp B/W/Y/R (163200-byte) frame. */
#define TESSERAE_DEVICE_KIND   "waveshare_1085g"
#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "bwry_4"

/* MCU tier: ESP32-S3 + embedded Quad PSRAM. Framebuffers live in SPIRAM. */
#define MCU_TIER_S3_QUAD_PSRAM 1

/* Selected panel driver: dual-controller 10.85-inch G. */
#define PANEL_DRIVER_WAVESHARE_1085G_DUAL 1
