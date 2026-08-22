/*
 * Board: Seeed reTerminal Sticky
 *   - MCU:   ESP32-S3, 8 MB PSRAM, 32 MB QSPI flash
 *   - Panel: 3.97" 800x480, 4-level grayscale, SSD1677 over SPI
 *   - Touch: capacitive, I2C
 *
 * Family E. Uses the ssd1677_gray driver.
 *
 * HOW THE CONTROLLER WAS IDENTIFIED. Seeed's Sticky documentation gives the
 * GPIO map but never names the display controller, and there is no Sticky entry
 * in Seeed_GFX. The pin map settles it: the documented panel pins here are
 * identical, pin for pin, to Seeed_GFX's Setup524_Seeed_reTerminal_E1005.h,
 * which declares SSD1677_DRIVER at 800x480. The Sticky and the (unannounced)
 * E1005 are the same board design, so the controller is SSD1677 -- and the same
 * driver should also cover the XIAO ePaper 3.97", whose Seeed_GFX setup names
 * the same part.
 *
 * UNVERIFIED ON HARDWARE. The pin map is from Seeed's docs and the sequences
 * are ported from Seeed_GFX, but nothing here has been run on a panel. Flash
 * the -selftest env first: four grey bands, black at the top through white at
 * the bottom. If the ends are swapped, the plane encoding in ssd1677_gray.c is
 * inverted for this glass; if nothing refreshes at all, suspect BUSY polarity
 * (this controller is busy-HIGH, the opposite of the UC8179 panels).
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (from Seeed's Sticky hardware overview).              */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   13
#define EPD_PIN_MOSI   14
#define EPD_PIN_CS     15
#define EPD_PIN_DC     16
#define EPD_PIN_RST    17
#define EPD_PIN_BUSY   18   /* active HIGH: 1 = busy (opposite of UC8179) */
#define EPD_PIN_EN     47   /* panel power enable */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (4 * 1000 * 1000)

/* Panel geometry. 800x480, 2bpp packed = 4 px/byte. Byte-identical in shape to
 * the E1001 gray build, which is why no server-side work was needed: the
 * renderer, the .bin packer and the gray_4 gamut already exist. */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 4)   /* 2bpp packed = 96000 */

/* 4-gray palette (linear), matching the E1001 gray board. */
#define EPD_COL_BLACK      0x0
#define EPD_COL_DARKGRAY   0x1
#define EPD_COL_LIGHTGRAY  0x2
#define EPD_COL_WHITE      0x3

/* Board model -> default device id "reTerminal_Sticky_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "reTerminal_Sticky"
/* Next free code; 1-7 are taken. Identifies the hardware over BLE setup. */
#define TESSERAE_BLE_HARDWARE_CODE  8

/* Tesserae hardware-catalog kind.
 *
 * SERVER DEPENDENCY: needs a catalog entry for seeed_reterminal_sticky. It can
 * be a copy of the E1001 gray entry with this id and 800x480 dims -- same
 * protocol (esp32_bw_client), same panel.gamut (gray_4), same renderer
 * (esp32_gray2_bin), because the frame bytes are the same shape. Without it the
 * panel pairs but never receives a correctly packed frame.
 *
 * NOTE: an existing Sticky may already be registered under a different kind by
 * whatever firmware it shipped with. Moving it to this one is a re-key, not an
 * upgrade. */
#define TESSERAE_DEVICE_KIND   "seeed_reterminal_sticky"

#define TESSERAE_RELAY_MODEL   "esp32_bw_client"
#define TESSERAE_RELAY_GAMUT   "gray_4"

/* Battery: the Sticky has a BQ27220 fuel gauge on I2C rather than a divider to
 * an ADC pin, so battery_present() is false until that driver exists. Reporting
 * nothing is correct meanwhile: a 0 mV reading would render as a permanently
 * flat cell (see the XIAO C3 panel for the same decision). */

/* Onboard SHT40 on the sensor I2C bus, shared with the IMU, RTC and gauge. */
#define BOARD_HAS_SHT4X            1
#define BOARD_SHT4X_I2C_PORT       0
#define BOARD_SHT4X_I2C_SDA        1
#define BOARD_SHT4X_I2C_SCL        0
#define BOARD_SHT4X_I2C_HZ         100000
#define BOARD_SHT4X_I2C_ADDR       0x44

/* Front buttons. The AI/power key doubles as the wake button. */
#define BOARD_BTN_REFRESH_PIN  4    /* AI / power */
#define BOARD_BTN_LEFT_PIN     5    /* up   */
#define BOARD_BTN_RIGHT_PIN    6    /* down */

/* microSD shares the panel SPI bus (SCLK 13 / MOSI 14) with its own CS; MISO is
 * SD-only, the panel being write-only. */
#define TESSERAE_SD_SLOT   1
#define SD_SPI_SHARED_BUS  1
#define SD_PIN_MISO   12
#define SD_PIN_CS     8

/* MCU tier: ESP32-S3 + 8 MB PSRAM. */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Selected panel driver: Family E, SSD1677 grayscale over SPI. */
#define PANEL_DRIVER_SSD1677_GRAY 1
