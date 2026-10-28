/*
 * Board: Waveshare ESP32-S3-RLCD-4.2
 *   - MCU:   ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM), native
 *            USB-Serial-JTAG on the Type-C port
 *   - Panel: 4.2" 400x300 fully reflective LCD (RLCD), Sitronix ST7305,
 *            1bpp, SPI with a DC line, no BUSY. Not e-paper: it holds the
 *            image only while powered, so the driver parks it in low-power
 *            mode across deep sleep instead of cutting a rail.
 *   - Power: 18650 holder, ETA6098 charger (CHG/WRN LEDs, no bus), TPS63020
 *            buck-boost to 3V3; cell on ADC via a 1:3 divider
 *   - Also:  SHTC3 temperature/humidity, PCF85063 RTC, microSD on SDMMC,
 *            ES8311/ES7210 audio with a dual microphone (unused), one user
 *            key, BOOT key, PWR slide switch (hardware, not a GPIO)
 *
 * Family F, the st7305_rlcd driver. Frame is the ordinary 400x300 1bpp mono
 * frame (15000 bytes), the same bytes the esp32_bw_bin renderer produces for
 * the Waveshare 4.2" e-paper, so the server side is a catalog entry.
 *
 * Pins from waveshareteam/ESP32-S3-RLCD-4.2 (examples/.../user_config.h,
 * sdcard_bsp.h, adc_bsp.cpp, button_bsp.c) and the ESP32-S3-RLCD-4.2 schematic
 * (LCD_SCL GPIO11, LCD_SDA GPIO12, LCD_RS GPIO5, LCD_CS GPIO40, LCD_RESET
 * GPIO41, LCD_TE GPIO6, BAT_ADC GPIO4, KEY GPIO18, SDA GPIO13 / SCL GPIO14).
 *
 * Verified on hardware 2026-10-27 (bench unit): selftest pattern, dashboard
 * frames over REST, image held through deep sleep, battery, SHTC3, KEY.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (single controller, SPI3, esp_lcd panel IO)           */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   11
#define EPD_PIN_MOSI   12
#define EPD_PIN_CS     40
#define EPD_PIN_DC     5    /* LCD_RS on the schematic */
#define EPD_PIN_RST    41
/* GPIO6 is the controller's tearing-effect output; nothing reads it. There is
 * no BUSY line: the ST7305 takes a frame whenever it is awake. */

#define EPD_SPI_HOST   SPI3_HOST   /* the host the vendor driver uses */
#define EPD_SPI_HZ     (10 * 1000 * 1000)   /* Waveshare's pclk */

/* The panel shows its image only while the ST7305 is powered and left alone,
 * so its lines are parked and HELD through every deep sleep: RST and CS high
 * (a dip on either blanks or deselects it), SCLK, MOSI and DC low so the
 * controller's inputs never float. main.c applies these on every sleep,
 * including wakes that found the frame unchanged and never touched the
 * driver -- the first bench deep sleep lost the image exactly that way. */
#define BOARD_SLEEP_DRIVE_HIGH_MASK  ((1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_CS))
#define BOARD_SLEEP_DRIVE_LOW_MASK   ((1ULL << EPD_PIN_SCLK) | (1ULL << EPD_PIN_MOSI) | (1ULL << EPD_PIN_DC))

/* Panel geometry. Landscape 400x300, 1bpp packed = 15000 bytes. The frame is
 * row-major with 8 px/byte; the driver transposes into the controller's own
 * 2x4 block layout on the way out. */
#define EPD_WIDTH      400
#define EPD_HEIGHT     300
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 8)   /* 1bpp packed = 15000 */

/* Mono palette (bit 1 = white), matching mono_spi and the splash. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1

/* Board model -> default device id "Waveshare_RLCD_42_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "Waveshare_RLCD_42"

/* Tesserae hardware-catalog kind (hardware/waveshare/waveshare_rlcd_42.json):
 * esp32_bw_client protocol, esp32_bw_bin renderer at 400x300. Distinct from
 * the e-paper waveshare_4_2_bw kind because kind names the OTA lineage. */
#define TESSERAE_DEVICE_KIND   "waveshare_rlcd_42"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair). */
#define TESSERAE_RELAY_MODEL   "esp32_bw_client"
#define TESSERAE_RELAY_GAMUT   "mono"

/* Battery sense: BAT_ADC on GPIO4 = ADC1 channel 3, through a divider that
 * Waveshare's adc_bsp.cpp scales by 3 (vol = pin_mv * 3). No load switch. */
#define BOARD_BATTERY_ADC_CHANNEL  ADC_CHANNEL_3
#define BOARD_BATTERY_DIVIDER      3

/* The one user key, KEY on GPIO18, active-low (button_bsp.c: GP18_Active 0).
 * GPIO18 is an RTC pad, so it wakes the S3 from deep sleep. Reported as
 * "right" so a stock server advances the rotation on a press, as the X4 does;
 * set to "refresh" for a plain repaint. BOOT (GPIO0) is left to the ROM. */
#define BOARD_BTN_REFRESH_PIN        18
#define BOARD_BTN_REFRESH_WIRE_NAME  "right"

/* Onboard SHTC3 temperature/humidity sensor on the ESP32_SDA/SCL bus, port 0,
 * SDA GPIO13 / SCL GPIO14 (user_config.h). Shared with the PCF85063 RTC and
 * the audio codecs. 100 kHz: nothing here needs more, and an unverified trace
 * gets the conservative rate. */
#define BOARD_HAS_SHTC3        1
#define BOARD_SHTC3_I2C_PORT   0
#define BOARD_SHTC3_I2C_SDA    13
#define BOARD_SHTC3_I2C_SCL    14
#define BOARD_SHTC3_I2C_HZ     100000
#define BOARD_SHTC3_I2C_ADDR   0x70

/* PCF85063 RTC (0x51, same bus, INT on GPIO15): NOT driven. Its register map
 * differs from the PCF8563 that rtc_pcf8563.c speaks (seconds at 0x04, not
 * 0x02; control bytes differ), so wiring BOARD_HAS_PCF8563 here would read
 * garbage. A PCF85063 driver is a follow-up; the firmware takes time from the
 * server as on boards without an RTC. */

/* microSD (deck cache): dedicated SDMMC pins, 1-bit. Waveshare's sdcard_bsp.h
 * defaults: clk=38, cmd=21, d0=39. No card-detect or slot power pin, so an
 * empty slot can only be found by probing; one attempt per mount (the
 * default three cost ~400 ms of retries on every wake with no card in). */
#define TESSERAE_SD_SLOT   1
#define SD_USE_SDMMC       1
#define SD_MMC_PIN_CLK     38
#define SD_MMC_PIN_CMD     21
#define SD_MMC_PIN_D0      39
#define SD_MOUNT_ATTEMPTS  1

/* MCU tier: ESP32-S3 + octal PSRAM (N16R8). */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Selected panel driver: Family F, ST7305 reflective LCD. */
#define PANEL_DRIVER_ST7305_RLCD 1
