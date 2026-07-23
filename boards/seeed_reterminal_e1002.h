/*
 * Board: Seeed reTerminal E1002
 *   - MCU:   XIAO ESP32-S3 class (ESP32-S3 + PSRAM)
 *   - Panel: 6-colour Spectra-6, single-controller, 800x480
 *
 * Family B. Uses the spectra6_spi_single driver, ported from a confirmed-working
 * ESP-IDF Spectra-6 update path. Single chip-select (no CS_M/CS_S split) and
 * no software power-enable pin. Pin map for BOARD_SEEED_RETERMINAL_E1002 and the
 * single-controller spectra6 init.
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
#define EPD_SPI_HZ     (20 * 1000 * 1000)   /* 20 MHz */

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

/* Tesserae hardware-catalog kind (esp32_client protocol + esp32_bin renderer:
 * 800x480 4bpp Spectra-6, 192000 bytes). The E1002 manifest was migrated to
 * this native path in Tesserae v0.64.52. */
#define TESSERAE_DEVICE_KIND   "seeed_reterminal_e1002"

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

/* Front buttons (reTerminal E baseboard). Middle "green" key on GPIO3 confirmed
 * (Seeed/TRMNL firmware wake/interrupt pin); left/right on GPIO5/GPIO4 from
 * Seeed's ESPHome reference, unverified. Active-low, RTC-capable, clear of the
 * panel pins. refresh->repaint, left->rotate prev, right->rotate next (see
 * buttons.h). GPIO3/4/5 = refresh/right/left VERIFIED on E1002 hardware directly
 * by button icon (2026-07-03); matches the E1001 baseboard. */
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

/* Selected panel driver: Family B, single-controller Spectra-6. */
#define PANEL_DRIVER_SPECTRA6_SPI_SINGLE 1
