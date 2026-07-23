/*
 * Board: Seeed reTerminal E1004
 *   - MCU:   XIAO ESP32-S3 class (ESP32-S3 + PSRAM)
 *   - Panel: 13.3" T133A01, dual-chip Spectra-6, 1200x1600
 *
 * IMPORTANT: despite being another "13.3" dual-controller Spectra-6 1200x1600"
 * panel, the T133A01 is NOT the same silicon as the Waveshare 13.3E6
 * (GDEP133C02). Its init sequence, refresh command (DRF=0x01), and per-frame
 * CCSET differ. It therefore uses its own driver (spectra6_t133a01_dual),
 * ported verbatim from the limengdu/bb_epaper fork (bbepT133A01InitIO /
 * bbepWriteImage4bppDual / bbepRefresh / bbepSleep) pinned at commit
 * 95fd94afe39cd7db32bef7c70eea06d654264ff6.
 *
 * Pin map for BOARD_SEEED_RETERMINAL_E1004.
 * Panel pins are named against the shared driver's roles:
 *   CS_M = primary controller (CS,  drives left  half, cols   0..599)
 *   CS_S = second  controller (CS1, drives right half, cols 600..1199)
 *   PWR  = EN, the active-high panel power enable
 *
 * UNVERIFIED ON HARDWARE. Confirm pins, PSRAM mode (oct vs quad), flash size
 * (esptool flash-id), and the init/refresh sequence on a real E1004 before
 * trusting a flash. See tesserae-device-firmware-HANDOVER.md.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (XIAO ESP32-S3 GPIO numbers)                          */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   7
#define EPD_PIN_MOSI   9
#define EPD_PIN_CS_M   10   /* primary controller (CS)  -> left  half  */
#define EPD_PIN_CS_S   2    /* second controller  (CS1) -> right half  */
#define EPD_PIN_DC     11
#define EPD_PIN_RST    38
#define EPD_PIN_BUSY   13
#define EPD_PIN_PWR    12   /* EN: active-high panel power enable       */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. Native orientation is portrait. Identical dimensions and
 * 4bpp packed layout to the base 13.3E6, so image_decoder/splash blobs and
 * the server-side 4bpp Spectra-6 renderer are reused unchanged. */
#define EPD_WIDTH      1200
#define EPD_HEIGHT     1600
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 4bpp packed = 960000 */

/* 6-color Spectra palette indices (nibble values). Same LUT as 13.3E6. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3
#define EPD_COL_BLUE    0x5
#define EPD_COL_GREEN   0x6

/* Board model -> default device id "reTerminal_E1004_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "reTerminal_E1004"

/* Tesserae hardware-catalog kind (esp32_client protocol + esp32_bin renderer:
 * raw 4bpp Spectra-6, EPD_WIDTH*EPD_HEIGHT/2 bytes). */
#define TESSERAE_DEVICE_KIND   "seeed_reterminal_e1004"

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

/* Front buttons -- VERIFIED on E1004 hardware by button icon (serial log,
 * 2026-07-03): the keys are plain active-low GPIOs (ext1 wakes fire), NOT a
 * capacitive/touch controller as feared. The pin->key wiring DIFFERS from the
 * E1001-3 baseboard: here Left=GPIO4, Right=GPIO3, Refresh=GPIO5 (on the E1001-3
 * it's Left=5/Right=4/Refresh=3). All clear of this board's panel pins (note
 * GPIO2 = EPD_PIN_CS_S, so the EE02's GPIO2 key does not apply). See buttons.h. */
#define BOARD_BTN_REFRESH_PIN  5
#define BOARD_BTN_RIGHT_PIN    3
#define BOARD_BTN_LEFT_PIN     4

/* microSD (deck cache): shares the panel SPI bus (SCLK 7 / MOSI 9) with its
 * own CS; MISO is SD-only (the panel is write-only). DET low = card present,
 * SD_EN high powers the slot (this model ships with a 16 GB card fitted).
 * From the Seeed reTerminal E10xx Arduino peripherals cookbook. */
#define TESSERAE_SD_SLOT   1
#define SD_SPI_SHARED_BUS  1
#define SD_PIN_MISO   8
#define SD_PIN_CS     14
#define SD_PIN_DET    15
#define SD_PIN_EN     16

/* ------------------------------------------------------------------ */
/* MCU tier: ESP32-S3 + PSRAM (assumed octal, verify on hardware).     */
/* ------------------------------------------------------------------ */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* ------------------------------------------------------------------ */
/* Selected panel driver (see src/panel/registry.c).                   */
/* Family A variant: Spectra-6 SPI dual-controller, T133A01 silicon.   */
/* ------------------------------------------------------------------ */
#define PANEL_DRIVER_SPECTRA6_T133A01_DUAL 1
