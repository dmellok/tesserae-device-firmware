/*
 * Board: epdiy v7 (vroland/epdiy) with an ED133UT2 13.3" panel
 *   - MCU:   ESP32-S3 module with octal PSRAM (the v7 board's WROOM-1)
 *   - Power: TPS65185 PMIC (panel rails, VCOM, thermistor) and a PCA9555 I/O
 *            expander for the control lines, both on I2C SDA 39 / SCL 40
 *   - Panel: ED133UT2, 1600x1200, 16-level greyscale, 8-bit parallel bus
 *
 * Family G: everything panel-side goes through the epdiy library's own v7
 * board definition (src/panel/drivers/epdiy_gray.c), so this header names the
 * epdiy board and display and the panel's settings; no pins of ours.
 *
 * Another panel on the same board is another epdiy display (see epdiy's
 * src/displays.c): change EPD_EPDIY_DISPLAY, EPD_WIDTH/EPD_HEIGHT and the VCOM,
 * and add a server catalog entry for the new size. No battery, no buttons, no
 * touch, no sensors. UNVERIFIED ON HARDWARE.
 */
#pragma once

/* epdiy objects (epd_board.h / epd_display.h). */
#define EPD_EPDIY_BOARD     epd_board_v7
#define EPD_EPDIY_DISPLAY   ED133UT2

/* VCOM in millivolts, as a positive number: the panel's flex cable is printed
 * with its own (e.g. "-1.56V" is 1560). A wrong VCOM gives washed-out greys or
 * ghosting rather than a blank panel. Override per unit with
 * -DEPD_EPDIY_VCOM_MV=... in build_flags. */
#ifndef EPD_EPDIY_VCOM_MV
#define EPD_EPDIY_VCOM_MV   1560
#endif

/* Waveform: unset uses the display's default (epdiy pairs ED133UT2 with its
 * ED097TC2 waveform). For darker blacks, -DEPD_EPDIY_WAVEFORM=\&epdiy_ED133UT2
 * tries epdiy's ED133UT2-specific one, or point it at your own waveform. */

/* Panel geometry. Landscape-native 1600x1200, 4bpp packed = 960000 bytes. */
#define EPD_WIDTH           1600
#define EPD_HEIGHT          1200
#define EPD_BUF_BYTES       ((EPD_WIDTH * EPD_HEIGHT) / 2)

#define EPD_COL_BLACK       0x0
#define EPD_COL_WHITE       0xF

#define EPD_EPDIY_PANEL_NAME "epdiy v7 ED133UT2 13.3\" (1600x1200, 4bpp)"

/* Board model -> default device id "EPDiy_V7_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "EPDiy_V7"

/* Tesserae hardware-catalog kind (hardware/epdiy/epdiy_v7_ed133ut2.json). */
#define TESSERAE_DEVICE_KIND   "epdiy_v7_ed133ut2"

#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "gray_16"

/* MCU tier: ESP32-S3 + octal PSRAM. */
#define MCU_TIER_S3_OCTAL_PSRAM 1

#define PANEL_DRIVER_EPDIY_GRAY 1
