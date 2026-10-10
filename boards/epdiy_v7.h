/*
 * Board: epdiy v7 (vroland/epdiy) with an ED133UT2 13.3" panel
 *   - MCU:   ESP32-S3 module with octal PSRAM (the v7 board's WROOM-1)
 *   - Power: TPS65185 PMIC (panel rails, VCOM) whose enables sit behind a
 *            PCA9535 I/O expander, both on I2C SDA 39 / SCL 40
 *   - Panel: ED133UT2, 1600x1200, 16-level greyscale, 8-bit parallel bus
 *
 * Family F, the same parallel_epd_gray driver as the PaperS3 (a FastEPD port),
 * with two board switches: EPD_PAR_POWER_TPS65185 for the PMIC power path and
 * EPD_PAR_ROW_START_V7 for the v7's gate-driver timing. Pin map, bus speed and
 * line padding are FastEPD's BB_PANEL_EPDIY_V7 entry; the grey matrix and VCOM
 * are from Soneliem's ED133UT2 support for FastEPD (bitbank2/FastEPD#44),
 * tuned on this glass.
 *
 * Another panel on the v7 is a new header like this one: its size, matrix and
 * VCOM (printed on the panel's flex), plus a server catalog entry. No battery,
 * no buttons, no touch, no sensors. UNVERIFIED ON HARDWARE.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Parallel bus (FastEPD BB_PANEL_EPDIY_V7)                            */
/* ------------------------------------------------------------------ */
#define EPD_PAR_DATA_PINS   {5, 6, 7, 15, 16, 17, 18, 8}
#define EPD_PAR_BUS_WIDTH   8
#define EPD_PAR_PCLK_HZ     (20 * 1000 * 1000)
#define EPD_PIN_CL          4    /* pixel clock; the i80 bus's WR line */
#define EPD_PIN_SPH         41   /* start pulse horizontal; the i80 bus's CS */
#define EPD_PIN_LE          42   /* latch shifted row into the driver outputs */
#define EPD_PIN_SPV         45   /* start pulse vertical (gate driver token) */
#define EPD_PIN_CKV         48   /* gate driver clock; one pulse = one row */
#define EPD_PIN_DC_DUMMY    0    /* the i80 driver wants a D/C pin; FastEPD gives it GPIO0 */
#define EPD_PAR_LINE_PADDING 16

#define EPD_PAR_ROW_START_V7    1

/* ------------------------------------------------------------------ */
/* Panel power: TPS65185 behind a PCA9535 expander                     */
/* ------------------------------------------------------------------ */
#define EPD_PAR_POWER_TPS65185  1
#define EPD_PAR_I2C_PORT        0
#define EPD_PAR_I2C_SDA         39
#define EPD_PAR_I2C_SCL         40

/* VCOM in millivolts, as a positive number. The ED133UT2 in bitbank2/FastEPD#44
 * runs at -2.28 V; a panel's own value is printed on its flex cable, and a
 * wrong one washes the greys out rather than blanking the panel. */
#ifndef EPD_VCOM_MV
#define EPD_VCOM_MV         2280
#endif

/* ------------------------------------------------------------------ */
/* Panel geometry: landscape-native 1600x1200, 4bpp = 960000 bytes     */
/* ------------------------------------------------------------------ */
#define EPD_WIDTH           1600
#define EPD_HEIGHT          1200
#define EPD_BUF_BYTES       ((EPD_WIDTH * EPD_HEIGHT) / 2)

#define EPD_COL_BLACK       0x0
#define EPD_COL_WHITE       0xF

#define EPD_PAR_PANEL_NAME  "epdiy v7 ED133UT2 13.3\" (1600x1200, 4bpp)"

/* 16 levels x 19 passes; 1 = darken, 2 = lighten, 0 = neutral. From
 * bitbank2/FastEPD#44 (u8ThirteenPointThreeMatrix), tuned on an ED133UT2. */
#define EPD_PAR_GRAY_MATRIX { \
    /*  0 */ 2, 2, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    /*  1 */ 2, 2, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, \
    /*  2 */ 2, 2, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 0, 1, 1, 1, \
    /*  3 */ 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 2, 0, \
    /*  4 */ 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 2, 1, 1, 2, 0, \
    /*  5 */ 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 2, 0, 1, 1, 2, 0, \
    /*  6 */ 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 1, 1, 2, 0, \
    /*  7 */ 1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 2, 0, 1, 1, 2, 0, \
    /*  8 */ 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 0, 1, 2, 0, \
    /*  9 */ 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 0, 1, 2, 0, \
    /* 10 */ 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 1, 2, 0, 1, 2, 0, \
    /* 11 */ 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 1, 1, 2, 0, 1, 2, 0, \
    /* 12 */ 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 2, 2, 2, 1, 2, 0, 1, 2, 0, \
    /* 13 */ 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 0, 1, 2, 0, \
    /* 14 */ 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 1, 0, 2, \
    /* 15 */ 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2 }

/* Board model -> default device id "EPDiy_V7_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "EPDiy_V7"

/* Tesserae hardware-catalog kind (hardware/epdiy/epdiy_v7_ed133ut2.json). */
#define TESSERAE_DEVICE_KIND   "epdiy_v7_ed133ut2"

#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "gray_16"

/* MCU tier: ESP32-S3 + octal PSRAM. */
#define MCU_TIER_S3_OCTAL_PSRAM 1

#define PANEL_DRIVER_PARALLEL_EPD_GRAY 1
