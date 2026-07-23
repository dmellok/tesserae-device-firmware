/*
 * Board: Seeed reTerminal E1001
 *   - MCU:   XIAO ESP32-S3 class (ESP32-S3 + PSRAM)
 *   - Panel: 7.5" mono (black/white), 800x480, UC8179-class (EP75)
 *
 * Family C. Uses the mono_spi driver. Same XIAO pins as the E1002, but the
 * panel is 1-bit monochrome, so EPD_BUF_BYTES is W*H/8 (48000) and the frame
 * is a packed 1bpp bitmap (bit 1 = white, bit 0 = black), not 4bpp Spectra.
 *
 * Panel = bb_epaper EP75_800x480 (BOARD_SEEED_RETERMINAL_E1001);
 * init/refresh ported into mono_spi.c. Pins from the reTerminal E1001 pin map.
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

/* Battery sense (reTerminal): GPIO1 = ADC1 channel 0, 2:1 divider,
 * gated by a load switch on GPIO21 (active-high, ~10 ms settle). */
#define BOARD_BATTERY_ADC_CHANNEL  ADC_CHANNEL_0
#define BOARD_BATTERY_DIVIDER      2
#define BOARD_VBAT_SWITCH_PIN      21

/* Onboard SHT4x environmental sensor shared by all reTerminal E models. */
#define BOARD_HAS_SHT4X            1
#define BOARD_SHT4X_I2C_PORT       0
#define BOARD_SHT4X_I2C_SDA        19
#define BOARD_SHT4X_I2C_SCL        20
#define BOARD_SHT4X_I2C_HZ         100000
#define BOARD_SHT4X_I2C_ADDR       0x44

/* Front buttons (reTerminal E baseboard). The middle "green" key on GPIO3 is
 * confirmed (Seeed/TRMNL firmware use it as the wake/interrupt pin); the
 * left/right keys on GPIO5/GPIO4 come from Seeed's ESPHome reference and are
 * unverified. All active-low, RTC-capable, and clear of the panel pins
 * (7/9/10/11/12/13). refresh->repaint, left->rotate prev, right->rotate next
 * (mapping is configurable server-side; see buttons.h).
 * VERIFIED on E1001 hardware by button icon (serial log, 2026-07-03): the
 * left/right/refresh keys report left/right/refresh respectively. Physical order
 * left-to-right is Left, Right, Refresh (green on the end, not the middle). */
#define BOARD_BTN_REFRESH_PIN  3
#define BOARD_BTN_RIGHT_PIN    4
#define BOARD_BTN_LEFT_PIN     5

/* microSD (deck cache): shares the panel SPI bus (SCLK 7 / MOSI 9) with its
 * own CS; MISO is SD-only (the panel is write-only). DET low = card present,
 * SD_EN high powers the slot. From the Seeed reTerminal E10xx Arduino
 * peripherals cookbook. Runtime-probed: no card -> feature dormant. */
#define TESSERAE_SD_SLOT   1
#define SD_SPI_SHARED_BUS  1
#define SD_PIN_MISO   8
#define SD_PIN_CS     14
#define SD_PIN_DET    15
#define SD_PIN_EN     16

/* MCU tier: ESP32-S3 + PSRAM (assumed octal; verify on hardware). */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Selected panel driver: Family C, single-controller mono SPI. */
#define PANEL_DRIVER_MONO_SPI 1
