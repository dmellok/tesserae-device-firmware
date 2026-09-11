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
#define TESSERAE_BLE_HARDWARE_CODE  1

/* Tesserae hardware-catalog kind. NOTE: the E1001 needs a MONO (1bpp 800x480)
 * server renderer, distinct from the Spectra esp32_bin path. Confirm this id
 * and that a matching manifest exists in your Tesserae. */
#define TESSERAE_DEVICE_KIND   "seeed_reterminal_e1001"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair).
 * Values come from this board's entry in the Tesserae hardware
 * catalog (hardware/<vendor>/seeed_reterminal_e1001.json): "protocol" is the
 * device KIND that selects the renderer/.bin packer, and
 * "panel.gamut" is the palette the server quantizes to. Both are
 * hardware facts, so the panel reports them at pairing and the
 * operator no longer has to pre-enter them.
 *
 * NOTE the kind is NOT the catalog id above -- that is a hardware id, not a
 * device kind. Getting this wrong silently mis-packs every frame. */
#define TESSERAE_RELAY_MODEL   "esp32_bw_client"
#define TESSERAE_RELAY_GAMUT   "mono"

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

/* PCF8563 real-time clock with a CR1220 backup, on the sensor bus. Address and
 * register map per Seeed's examples/base/RTC_PCF8563/RTC_PCF8563.ino, which
 * names SDA 19 / SCL 20 for every E1001-E1004. Holds UTC (rtc_pcf8563.h). */
#define BOARD_HAS_PCF8563          1
#define BOARD_PCF8563_I2C_PORT     0
#define BOARD_PCF8563_I2C_SDA      19
#define BOARD_PCF8563_I2C_SCL      20
#define BOARD_PCF8563_I2C_HZ       400000
#define BOARD_PCF8563_I2C_ADDR     0x51

/* SY6974 charger, status read only (sy6974.h never writes it). On this board
 * it sits on its OWN bus, I2C port 1 on SDA 39 / SCL 40, not the sensor bus:
 * SenseCraft src/boards/reterminal_e1001/config.h maps PMIC_I2C_SDA/SCL to
 * ESP32_SDA1/SCL1 (39/40) for the E1001 and E1002 alike. GPIO39/40 are not
 * used by anything else in this header. */
#define BOARD_HAS_SY6974           1
#define BOARD_SY6974_I2C_PORT      1
#define BOARD_SY6974_I2C_SDA       39
#define BOARD_SY6974_I2C_SCL       40
#define BOARD_SY6974_I2C_HZ        400000
#define BOARD_SY6974_I2C_ADDR      0x6B

/* Green status LED, active-low (the pin sinks; LOW = on). Seeed's
 * examples/base/LED_Control/LED_Control.ino: GPIO6 on the E1001/E1002. */
#define BOARD_LED_PIN              6
#define BOARD_LED_ACTIVE_LOW       1

/* Front buttons (reTerminal E baseboard). The middle "green" key on GPIO3 is
 * confirmed (Seeed/TRMNL firmware use it as the wake/interrupt pin); the
 * left/right keys on GPIO5/GPIO4 come from Seeed's ESPHome reference and are
 * unverified. All active-low, RTC-capable, and clear of the panel pins
 * (7/9/10/11/12/13). refresh->repaint, left->rotate prev, right->rotate next
 * (mapping is configurable server-side; see buttons.h).
 * VERIFIED on E1001 hardware by button icon (serial log, 2026-07-03): the
 * left/right/refresh keys report left/right/refresh respectively. Physical order
 * left-to-right is Left, Right, Refresh (green on the end, not the middle). */
/* Passive piezo on the reTerminal E baseboard, driven by LEDC PWM: frequency
 * sets the pitch, duty sets the loudness (server #258). Pin per Seeed's own
 * ESPHome cookbook for this series, which also names the buttons on GPIO3/4/5
 * exactly as this header does. Sounded locally on an input so the user gets
 * confirmation before the e-ink repaint, which takes seconds. */
#define BOARD_BUZZER_PIN           45

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
/* SDSPI data clock. IDF's default is 20 MHz; Seeed's own SD code never runs
 * this shared bus above 4 MHz, the E1004 already sits at 10, and firmware
 * #34 showed a card that passes init and then times out on its first data
 * read (the SD status block, which runs at the full clock). Start at 10 MHz;
 * the mount retry ladder steps to 4 then 1 MHz. Bench E1002 2026-09-11 was
 * fine at 20 with a 16 GB SDHC, so this is margin, not a fix for that unit. */
#define SD_SPI_MAX_KHZ 10000

/* Rails the firmware never uses, driven low + held through deep sleep
 * (main.c sleep_park_rails): GPIO38 enables the PDM microphone's TPS22916
 * load switch (Seeed MicRecordToSD.ino). Left floating it can leak the mic
 * rail on for the whole sleep. Bench 2026-09-11: gain unmeasured. */
#define BOARD_SLEEP_DRIVE_LOW_MASK  (1ULL << 38)

/* MCU tier: ESP32-S3 + PSRAM (assumed octal; verify on hardware). */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Selected panel driver: Family C, single-controller mono SPI. */
#define PANEL_DRIVER_MONO_SPI 1
