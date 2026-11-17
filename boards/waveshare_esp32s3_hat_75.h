/*
 * Board: ESP32-S3-DevKitC-1 + a Waveshare 7.5" 800x480 mono e-Paper HAT
 * (7.5inch e-Paper V2 / 7.50inV2p, UC8179-class, single controller, 1bpp).
 *
 *   - MCU:   ESP32-S3-DevKitC-1, 4 MB flash, NO PSRAM (device 0x4016),
 *            native USB-Serial-JTAG on the "USB" port, 512 KB SRAM
 *   - Panel: 7.5" 800x480 mono B/W, UC8179-class, driven 4-wire SPI
 *            (SCLK/MOSI/CS/DC + BUSY + RST, no MISO)
 *
 * The S3 sibling of waveshare_esp32_driver_75.h: identical glass, identical
 * 48000-byte 1bpp frame, different MCU and a hand-wired pin map. The pin map
 * below is the wiring ESPHome's reference `waveshare_epaper` config uses for
 * this HAT, observed working on real hardware.
 *
 * Pins (matching that reference wiring; GPIO 4/5 are also the S3's default
 * I2C pins, unused here):
 *
 *   CLK  GPIO4    DIN/MOSI  GPIO5
 *   CS   GPIO6    DC        GPIO7
 *   BUSY GPIO8    RST       GPIO9
 *
 * BUSY is ACTIVE LOW on this glass (0 = busy), which is why the ESPHome config
 * carried `inverted: true` on that pin. Keep that polarity: the driver waits
 * for BUSY high, and driving a panel while it says it is busy is what can
 * damage it. Do not "fix" this by flipping the define.
 *
 * No battery sense, no user buttons (EN/BOOT only), no PSRAM, no microSD.
 * Setup is the captive portal; a reflash over USB is the recovery path.
 *
 * OTA is OFF in this env on purpose. The device kind below is the classic
 * ESP32 board's, because that is the catalog entry for this exact glass -- but
 * its OTA lineage ships Xtensa LX6 images, and an S3 running one would not
 * boot. Advertising OTA against that kind would therefore invite a wrong
 * image; without TESSERAE_OTA_CAPABILITY_ENABLED the server never offers one
 * and the panel stays on whatever we flash over USB.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (ESP32-S3-DevKitC-1, hand-wired to the HAT)           */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   4
#define EPD_PIN_MOSI   5
#define EPD_PIN_CS     6
#define EPD_PIN_DC     7
#define EPD_PIN_RST    9
#define EPD_PIN_BUSY   8    /* active low: 0 = busy */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. 800x480 mono, packed 1bpp = W*H/8 = 48000 bytes. */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 8)   /* 1bpp packed = 48000 */

/* Mono palette (bit 1 = white, bit 0 = black), matching mono_spi / the E1001. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1

/* Board model -> default device id "Waveshare_ESP32S3_HAT_75_<mac-suffix>".
 * Not the catalog's model_header string: that one names the classic driver
 * board, and this is a devkit wired to the same HAT. */
#define TESSERAE_DEVICE_MODEL  "Waveshare_ESP32S3_HAT_75"

/* Tesserae hardware-catalog kind. The 7.5" mono glass is
 * hardware/waveshare/waveshare_esp32_driver_75.json: 800x480, mono,
 * esp32_bw_client -> esp32_bw_bin renderer, exactly 48000 bytes, MSB=leftmost,
 * bit-set=white. Byte-identical to the E1001 / XIAO 7.5" builds, so the server
 * side is settled; only the MCU differs. */
#define TESSERAE_DEVICE_KIND   "waveshare_esp32_driver_75"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair). Same mono
 * frame as the other 800x480 1bpp boards. */
#define TESSERAE_RELAY_MODEL   "esp32_bw_client"
#define TESSERAE_RELAY_GAMUT   "mono"

/* No battery sense: mains-powered on the wall, and the devkit has no divider.
 * Leaving BOARD_BATTERY_ADC_CHANNEL undefined makes battery_read_mv() return 0,
 * which the server reads as "unknown" rather than "empty". */

/* No user buttons beyond EN and BOOT, so no BOARD_BTN_REFRESH_PIN and no
 * Bluetooth setup gesture. Setup is the captive portal. */

/* MCU tier, descriptive only (no code keys off these macros; the real switch
 * is CONFIG_SPIRAM in the sdkconfig base). S3, no PSRAM: the 48000-byte frame
 * comes from internal RAM, which is why this header leaves EPD_DUAL_DEPTH
 * undefined -- a 96000-byte grey frame beside WiFi and TLS has not been
 * checked on this part, and the panel stays mono on the server anyway. */
#define MCU_TIER_S3_NO_PSRAM 1

/* Selected panel driver: Family C, single-controller mono (shared w/ E1001). */
#define PANEL_DRIVER_MONO_SPI 1
