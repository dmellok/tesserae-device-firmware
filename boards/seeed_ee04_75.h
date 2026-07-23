/*
 * Board: Seeed Studio XIAO ePaper Display Board EE04 + 7.5" mono panel
 *   - MCU:   XIAO ESP32-S3 Plus (ESP32-S3 + PSRAM), native USB (no CH340)
 *   - Panel: 7.5" 800x480 mono B/W (UC8179-class), single controller, 1bpp,
 *     on the EE04's 24-pin FPC (jumper caps must be in the 24-pin position)
 *
 * The EE04 hosts one single-controller panel on either a 24-pin or a 50-pin
 * FPC; this header is the 24-pin 7.5" mono build (see seeed_ee04_73e6.h for
 * the 50-pin Spectra-6 variant). Same panel class as the E1001 / XIAO 7.5"
 * kit, so it REUSES the mono_spi driver unchanged.
 *
 * Pin map from Seeed_GFX (User_Setups/EPaper_Board_Pins_Setups.h,
 * USE_XIAO_EPAPER_DISPLAY_BOARD_EE04) -- the single-CS subset of the EE02 map:
 *   SCLK=D8=GPIO7, MOSI=D10=GPIO9, CS=44, DC=10, BUSY=4, RST=38, ENABLE=43.
 *
 * NOTE: CS (44) and ENABLE (43) are the ESP32-S3 UART0 RX/TX pins, so this
 * env routes the console to USB-Serial-JTAG (sdkconfig.usbjtag.defaults).
 *
 * TODO(verify): whole board is community bring-up hardware -- pins compile-
 * checked against Seeed_GFX but not yet run on a real EE04.
 *
 * Frame is the same 48000-byte 800x480 1bpp packed layout as the E1001, so
 * the server reuses that mono renderer under TESSERAE_DEVICE_KIND below.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (single controller, SPI2) -- Seeed EE04 GPIO numbers   */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   7    /* XIAO D8  */
#define EPD_PIN_MOSI   9    /* XIAO D10 */
#define EPD_PIN_CS     44
#define EPD_PIN_DC     10
#define EPD_PIN_RST    38
#define EPD_PIN_BUSY   4    /* active low: 0 = busy */
#define EPD_PIN_PWR    43   /* TFT_ENABLE: active-high panel power enable.
                             * The XIAO 7.5" kit works with this pin left
                             * floating, but the EE02 firmware drives it, so
                             * drive it here too for a deterministic rail. */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. 800x480 mono, packed 1bpp = W*H/8 = 48000 bytes. */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 8)   /* 1bpp packed = 48000 */

/* Mono palette (bit 1 = white, bit 0 = black), matching mono_spi / the E1001. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1

/* Board model -> default device id "Seeed_EE04_75_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "Seeed_EE04_75"

/* Tesserae hardware-catalog kind. Same 800x480 1bpp mono frame (48000 bytes)
 * as the E1001, so the server maps this to the existing mono renderer. */
#define TESSERAE_DEVICE_KIND   "seeed_ee04_75"

/* Battery sense: GPIO1 = ADC1 channel 0, 2:1 divider, gated by a load switch
 * on GPIO6 (active-high) -- documented on the EE04 wiki (VBAT sense A0/GPIO1,
 * ADC enable A5/GPIO6), matching the circuit isolated on the XIAO 7.5" kit. */
#define BOARD_BATTERY_ADC_CHANNEL  ADC_CHANNEL_0
#define BOARD_BATTERY_DIVIDER      2
#define BOARD_VBAT_SWITCH_PIN      6

/* Front buttons: KEY1/KEY2/KEY3 on GPIO2/3/5 per the EE04 wiki (all active-
 * low, RTC-capable -> ext1 deep-sleep wake sources). Same assignment as the
 * EE02: Key1=refresh, Key2=left, Key3=right (see buttons.h). */
#define BOARD_BTN_REFRESH_PIN  2
#define BOARD_BTN_LEFT_PIN     3
#define BOARD_BTN_RIGHT_PIN    5

/* MCU tier: ESP32-S3 + octal PSRAM. */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Selected panel driver: Family C, single-controller mono (shared w/ E1001). */
#define PANEL_DRIVER_MONO_SPI 1
