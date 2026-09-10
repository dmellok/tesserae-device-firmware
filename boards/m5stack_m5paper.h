/*
 * Board: M5Stack M5Paper (SKU K097, the original / pre-S3 M5Paper, v1.1)
 *   - MCU:   ESP32-D0WDQ6-V3 (classic Xtensa LX6, 16 MB flash, 8 MB QUAD
 *            PSRAM -- 4 MB of it mapped, the classic ESP32's address-space
 *            limit -- UART0 console over a CH9102 bridge, no native USB)
 *   - Panel: 4.7" ED047TC1, 960x540, 16-level greyscale, via an IT8951
 *            timing controller over SPI
 *
 * Family D. Reuses the it8951_gray driver unchanged in shape from the Seeed
 * reTerminal E1003: the ESP32 talks to an IT8951 over a 16-bit-word SPI
 * protocol, the IT8951 drives the panel's parallel glass internally, driven
 * in 4bpp (16-level) grayscale with the GC16 waveform.
 *
 * This is the pre-S3 sibling of the M5Stack PaperS3 (boards/m5stack_papers3.h).
 * Same 4.7" glass, but the PaperS3 bit-bangs the bare panel over the S3's i80
 * bus (Family F) while this board reaches it through the IT8951 over SPI, on
 * classic-ESP32 silicon that has no i80 peripheral at all.
 *
 * Pin map, geometry and power sequence from M5Stack's own libraries, which
 * agree pin for pin: M5GFX src/M5GFX.cpp (board_M5Paper autodetect +
 * Panel_IT8951 cfg), M5Unified src/utility/Power_Class.cpp (battery ADC) and
 * src/M5Unified.cpp (buttons), M5EPD src/M5EPD.h (the legacy #defines).
 *
 * VERIFIED ON HARDWARE 2026-09-10: panel (selftest grey ramp + a live
 * dashboard frame over REST, upright and unmirrored), battery telemetry, the
 * three side buttons, the SHT30 sensor, and GT911 touch dispatch through the
 * server. Not wired: microSD (deck cache), and deep-sleep wake for touch or
 * the outer buttons -- the classic ESP32's ext1 has no ANY_LOW mode.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* IT8951 SPI pin map (classic ESP32 GPIO numbers)                     */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK    14
#define EPD_PIN_MOSI    12
#define EPD_PIN_MISO    13   /* IT8951 is bidirectional (device-info reads) */
#define EPD_PIN_CS      15
#define EPD_PIN_BUSY    27   /* HRDY: HIGH = ready (external pull-up on this board) */

/* M5Paper has ONE line for the T-CON / panel power, GPIO23, and no separate
 * IT8951 reset pin: a power-cycle of GPIO23 is the reset (M5GFX drives GPIO23
 * as Panel_IT8951::pin_rst; M5EPD names the same net M5EPD_EPD_PWR_EN_PIN).
 * The it8951_gray driver toggles EPD_PIN_RST, EPD_PIN_EN and EPD_PIN_VCC_EN in
 * hw_reset_and_power(); collapsing all three onto GPIO23 turns that into a
 * clean off / on / reset-pulse of the one rail this board exposes. */
#define EPD_PIN_RST     23
#define EPD_PIN_EN      23
#define EPD_PIN_VCC_EN  23

#define EPD_SPI_HOST    SPI2_HOST
/* 10 MHz, M5EPD's value (M5GFX pushes 40/20). Conservative: MOSI/MISO are not
 * on the HSPI IOMUX so the bus runs through the GPIO matrix. Fine on hardware. */
#define EPD_SPI_HZ      (10 * 1000 * 1000)

/* Panel geometry. 960x540, 4bpp packed greyscale = 2 px/byte = 259200 bytes. */
#define EPD_WIDTH       960
#define EPD_HEIGHT      540
#define EPD_BUF_BYTES   ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 259200 */

/* Grey nibbles: 0x0 = black ... 0xF = white, matching the shared 4bpp splash
 * path and the server's esp32_gray_bin wire format. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0xF

/* IT8951 VCOM: 0 = keep the factory value in the controller's OTP (what M5GFX
 * does). The M5Paper's stored VCOM is correct for its glass; the E1003 path
 * overrides it only because FastEPD does. The driver logs the stored value. */
#define EPD_VCOM_MV     0

/* Waveform-LUT temperature (IT8951 CMD_TEMP). The driver's 14 C default is
 * FastEPD's E1003 value; a handheld runs warmer, and too cold a LUT
 * under-drives the mid greys, so this board uses 22 C. */
#define EPD_IT8951_FORCE_TEMP_C  22

/* X-mirror off. The driver defaults EPD_IT8951_MIRROR_X to 1 for the E1003 /
 * EE03 (ED103TC2 glass is physically left-right mirrored); the M5Paper's
 * ED047TC1 is not (M5GFX drives it with no MIRROR_X), so rows stream straight
 * through. */
#define EPD_IT8951_MIRROR_X  0

/* Repaint floor while always-on. The shared default (30 s) is for Spectra-6
 * colour glass; the IT8951's GC16 is ~1.1 s, so a held-back Send is pure
 * latency. 4 s still rate-limits the glass. */
#define AWAKE_MIN_REPAINT_S  4

/* ------------------------------------------------------------------ */
/* Power: battery self-latch                                           */
/* ------------------------------------------------------------------ */
/* GPIO2 (M5EPD_MAIN_PWR_PIN) holds the main rail on after the side power
 * button is released. Drive HIGH first thing in app_main() via
 * power_latch_hold(), or everything runs only while the button is held and
 * the board dies the moment the user lets go. Latch it through deep sleep too,
 * or a timed wake becomes a power-off. */
#define BOARD_POWER_LATCH_PIN   2

/* GPIO5 (M5EPD_EXT_PWR_EN_PIN) gates the Grove Port A 5 V boost. Left low:
 * nothing on this firmware drives the external port, and the boost is pure
 * drain on battery. Recorded, not driven. */

/* ------------------------------------------------------------------ */
/* Battery                                                             */
/* ------------------------------------------------------------------ */
/* GPIO35 = ADC1 channel 7, fixed 2:1 divider, no load switch. 1150 mAh Li-Po.
 * From M5Unified Power_Class (_batAdcPin 35, _adc_ratio 2.0). battery.c
 * calibrates with line fitting on this target -- the classic ESP32 has no
 * curve-fitting scheme. */
#define BOARD_BATTERY_ADC_CHANNEL  ADC_CHANNEL_7
#define BOARD_BATTERY_DIVIDER      2

/* ------------------------------------------------------------------ */
/* Buttons -- three physical side keys                                   */
/* ------------------------------------------------------------------ */
/* GPIO37 / 38 / 39 (M5Unified: A / B / C = LEFT / PUSH / RIGHT on the long
 * edge), active-low with external pull-ups, all RTC-capable. Centre -> refresh,
 * outer two -> rotate prev / next.
 *
 * Deep-sleep wake is limited here: the classic ESP32's ext1 has no ANY_LOW
 * mode and no GPIO deep-sleep wake, so buttons.h falls back to ext0 on the
 * refresh pin only. The centre key wakes; left/right work only while awake
 * (always-on, or the post-press linger). */
#define BOARD_BTN_LEFT_PIN     37
#define BOARD_BTN_REFRESH_PIN  38
#define BOARD_BTN_RIGHT_PIN    39

/* ------------------------------------------------------------------ */
/* Touch -- Goodix GT911 on the internal I2C bus                          */
/* ------------------------------------------------------------------ */
/* No TP_RST to the MCU (M5GFX's Touch_GT911 cfg has no pin_rst), so
 * BOARD_TOUCH_RST_PIN is undefined: touch_gt911.c skips the reset/address-strap
 * sequence and just reads the controller, which is permanently powered. The
 * address is not deterministic without the strap -- this unit answered at 0x5d,
 * the driver falls back to 0x14 (M5GFX alternates the same way).
 *
 * BOARD_TOUCH_FRAME_W/H are the COMPOSITION dims (540x960), not the 960x540
 * panel: esp32_gray_bin rotates the frame server-side and the server hit-tests
 * taps in composition space. The GT911's axes sit 90 degrees to it, so
 * SWAP_XY + INVERT_X align them.
 *
 * Touch works only while awake -- deep-sleep touch wake needs the ext1 ANY_LOW
 * the classic ESP32 lacks. In always-on mode main.c polls the GT911. */
#define BOARD_HAS_TOUCH           1
#define BOARD_TOUCH_I2C_PORT      0
#define BOARD_TOUCH_I2C_SDA       21
#define BOARD_TOUCH_I2C_SCL       22
#define BOARD_TOUCH_I2C_HZ        400000
#define BOARD_TOUCH_I2C_ADDR      0x5d
#define BOARD_TOUCH_INT_PIN       36
#define BOARD_TOUCH_FRAME_W       540
#define BOARD_TOUCH_FRAME_H       960
#define BOARD_TOUCH_SWAP_XY       1
#define BOARD_TOUCH_INVERT_X      1
#define BOARD_TOUCH_INVERT_Y      0

/* ------------------------------------------------------------------ */
/* SHT30 temperature / humidity, on the internal I2C bus at 0x44         */
/* ------------------------------------------------------------------ */
/* Shares the internal I2C bus with the GT911 (routed through i2c_bus_get) and
 * the BM8563 RTC. Read once per heartbeat by net_rest.c -> sht3x_read(); the
 * SHT3x single-shot command set differs from the SHT4x and SHTC3, hence a
 * third driver. The RTC stays unused -- the server bakes local time into
 * every frame. */
#define BOARD_HAS_SHT3X            1
#define BOARD_SHT3X_I2C_PORT       0
#define BOARD_SHT3X_I2C_SDA        21
#define BOARD_SHT3X_I2C_SCL        22
#define BOARD_SHT3X_I2C_HZ         100000
#define BOARD_SHT3X_I2C_ADDR       0x44

/* ------------------------------------------------------------------ */
/* microSD (deck cache) -- present, NOT enabled yet                       */
/* ------------------------------------------------------------------ */
/* The slot's CS is GPIO4 and it shares the panel SPI bus (SCLK 14 / MOSI 12 /
 * MISO 13), exactly the topology the E1003 header calls out as fragile: the
 * IT8951 hangs off the same MISO, so a loaded bus needs SD_SPI_MAX_KHZ well
 * below the SDSPI default or bulk reads fail. Left off for bring-up -- the
 * deck cache is an optimisation, not a requirement. To enable later:
 *   #define TESSERAE_SD_SLOT   1
 *   #define SD_SPI_SHARED_BUS  1
 *   #define SD_SPI_MAX_KHZ     10000
 *   #define SD_PIN_MISO        13
 *   #define SD_PIN_CS          4
 */

/* Board model -> default device id "M5Paper_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "M5Paper"
#define TESSERAE_BLE_HARDWARE_CODE  0   /* no BLE overlay on classic ESP32 build */

/* Tesserae hardware-catalog kind. Selects the server-side renderer and .bin
 * packer: esp32_gray_bin, the same 4bpp linear-greyscale wire format the E1003
 * and PaperS3 use, at this panel's 960x540 geometry. A kind is also the OTA
 * lineage, so it is fixed from first release. See
 * hardware/m5stack/m5paper.json in the Tesserae repo. */
#define TESSERAE_DEVICE_KIND   "m5stack_m5paper"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair). "protocol"
 * is the device kind's transport family, "gamut" the palette the server
 * quantizes to. NOTE the relay model is NOT the catalog kind above. The
 * catalog entry marks this SKU auto_select:false -- an M5Paper and a PaperS3
 * report the same model + gamut + 960x540 scan, so the server pins this kind
 * only from TESSERAE_DEVICE_KIND declared at register/discover, never from
 * relay gamut inference. */
#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "gray_16"

/* MCU tier: classic ESP32 + quad PSRAM (8 MB fitted, 4 MB mapped -- the
 * classic ESP32's external-RAM address window). The 259200-byte frame buffer
 * does not fit in the ~180 KB of internal DRAM left after WiFi + TLS, so
 * unlike the other classic-ESP32 board (Waveshare driver board, mono, 48000
 * bytes, no PSRAM) this one requires PSRAM -- see sdkconfig.esp32-psram.defaults
 * and TESSERAE_FB_CAPS in app_config.h. */
#define MCU_TIER_ESP32_QUAD_PSRAM 1

/* No BOARD_OVERLAY_PARTIAL: the IT8951's DU waveform could drive tap echo /
 * value slots (the E1003 does), but touch here is always-on only and that is
 * a follow-up. */

/* Selected panel driver: Family D, IT8951 grayscale over SPI (shared w/ E1003). */
#define PANEL_DRIVER_IT8951_GRAY 1
