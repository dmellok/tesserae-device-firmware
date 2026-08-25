/*
 * Board: Seeed Studio XIAO ePaper Display Board EE05 + 2.13" BWRY panel
 *   - MCU:   XIAO ESP32-S3 Plus (ESP32-S3 + PSRAM), native USB (no CH340)
 *   - Panel: 2.13" quadruple-colour BWRY (black/white/yellow/red), JD79676
 *     controller, 122x250 visible on a 128x250 native stride, 24-pin FPC
 *
 * Pin map from Seeed_GFX (User_Setups/EPaper_Board_Pins_Setups.h,
 * USE_XIAO_EPAPER_DISPLAY_BOARD_EE05) -- byte-for-byte the EE04 single-CS
 * map, which is itself the single-CS subset of the EE02 map:
 *   SCLK=D8=GPIO7, MOSI=D10=GPIO9, CS=44, DC=10, BUSY=4, RST=38, ENABLE=43.
 *
 * NOTE: CS (44) and ENABLE (43) are the ESP32-S3 UART0 RX/TX pins, so this
 * env routes the console to USB-Serial-JTAG (sdkconfig.usbjtag.defaults).
 *
 * Geometry: the JD79676 consumes the full 128-column stride; only 122
 * columns reach the glass (Seeed_GFX COL_OFFSET 6). The server packs at the
 * native stride and pads the hidden columns (manifest col_offset), so
 * EPD_WIDTH here is the STRIDE, not the glass. Which edge hides the six
 * columns is confirmed by the selftest pattern (see jd79676_bwry.c).
 *
 * TODO(verify): community bring-up hardware (tesserae-device-firmware
 * issue #29) -- pins compile-checked against Seeed_GFX but not yet run on a
 * real EE05.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (single controller, SPI2) -- Seeed EE05 GPIO numbers   */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   7    /* XIAO D8  */
#define EPD_PIN_MOSI   9    /* XIAO D10 */
#define EPD_PIN_CS     44
#define EPD_PIN_DC     10
#define EPD_PIN_RST    38
#define EPD_PIN_BUSY   4    /* active low: 0 = busy */
#define EPD_PIN_PWR    43   /* TFT_ENABLE: active-high panel power enable */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry: native 128x250 2bpp stride = 128*250*2/8 = 8000 bytes.
 * EPD_VISIBLE_WIDTH is informational (122 columns of glass). */
#define EPD_WIDTH      128
#define EPD_HEIGHT     250
#define EPD_VISIBLE_WIDTH 122
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 4)   /* 2bpp packed = 8000 */

/* Server bwry_4 palette indices (the driver translates to JD79676 wire
 * codes internally; these are the values the frame/wire format uses). */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3

/* Board model -> default device id "Seeed_EE05_213_BWRY_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "Seeed_EE05_213_BWRY"
#define TESSERAE_BLE_HARDWARE_CODE  9

/* Tesserae hardware-catalog kind (hardware/seeed/ee05_213_bwry.json). */
#define TESSERAE_DEVICE_KIND   "seeed_ee05_213_bwry"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair).
 * "protocol" is the device KIND that selects the renderer/.bin packer, and
 * "panel.gamut" is the palette the server quantizes to. Both are hardware
 * facts, so the panel reports them at pairing and the operator no longer
 * has to pre-enter them.
 *
 * NOTE the kind is NOT the catalog id above -- that is a hardware id, not a
 * device kind. Getting this wrong silently mis-packs every frame. */
#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "bwry_4"

/* Battery sense: GPIO1 = ADC1 channel 0, 2:1 divider, gated by a load switch
 * on GPIO6 (active-high) -- the XIAO ePaper display-board circuit shared by
 * the EE02/EE04. TODO(verify): confirm on a real EE05 with the ADC sweep. */
#define BOARD_BATTERY_ADC_CHANNEL  ADC_CHANNEL_0
#define BOARD_BATTERY_DIVIDER      2
#define BOARD_VBAT_SWITCH_PIN      6

/* Front buttons: KEY1/KEY2/KEY3 on GPIO2/3/5 as on the EE02/EE04 (all
 * active-low, RTC-capable -> ext1 deep-sleep wake sources). Key1=refresh,
 * Key2=left, Key3=right (see buttons.h). TODO(verify) on a real EE05. */
#define BOARD_BTN_REFRESH_PIN  2
#define BOARD_BTN_LEFT_PIN     3
#define BOARD_BTN_RIGHT_PIN    5

/* MCU tier: ESP32-S3 + octal PSRAM. */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Selected panel driver: JD79676 quadruple-colour BWRY, single controller. */
#define PANEL_DRIVER_JD79676_BWRY 1
