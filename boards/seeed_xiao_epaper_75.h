/*
 * Board: Seeed XIAO ePaper 7.5" (mono) -- the panel in the "TRMNL 7.5\" OG DIY
 * Kit" (XIAO ESP32-S3 Plus + Seeed "ePaper Driver Board for XIAO V2" + a 7.5"
 * 800x480 monochrome e-paper).
 *
 *   - MCU:   XIAO ESP32-S3 Plus (ESP32-S3 + PSRAM), native USB (no CH340)
 *   - Panel: 7.5" 800x480 mono B/W (UC8179-class), single controller, 1bpp
 *
 * The panel is the same 800x480 mono class as the reTerminal E1001, so this
 * board REUSES the mono_spi driver unchanged. The kit uses the same XIAO
 * ESP32-S3 Plus ePaper driver board as the EE02 -- the single-CS subset of its
 * pin map (verified on hardware): SCLK=7, MOSI=9, CS=44, DC=10, BUSY=4, RST=38.
 * (My first guess used Seeed_GFX's *generic* driver-board pins, CS=2/DC=4/
 * BUSY=3/RST=1; the panel stayed blank -- wait_busy never blocked -- because
 * BUSY/CS were wrong.)
 *
 * NOTE: CS is GPIO44 (UART0 RX), so this env routes the console to
 * USB-Serial-JTAG (sdkconfig.usbjtag.defaults) to free UART0 -- and logs still
 * come out the XIAO's USB-C during bring-up.
 *
 * Frame is the same 48000-byte 800x480 1bpp packed layout as the E1001, so the
 * server reuses that mono renderer under TESSERAE_DEVICE_KIND below.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (single controller, SPI2) -- XIAO S3 Plus driver board */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   7    /* XIAO D8  */
#define EPD_PIN_MOSI   9    /* XIAO D10 */
#define EPD_PIN_CS     44
#define EPD_PIN_DC     10
#define EPD_PIN_RST    38
#define EPD_PIN_BUSY   4    /* active low: 0 = busy */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. 800x480 mono, packed 1bpp = W*H/8 = 48000 bytes. */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 8)   /* 1bpp packed = 48000 */

/* Mono palette (bit 1 = white, bit 0 = black), matching mono_spi / the E1001. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1

/* Board model -> default device id "XIAO_ePaper_75_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "XIAO_ePaper_75"

/* Tesserae hardware-catalog kind. Same 800x480 1bpp mono frame (48000 bytes)
 * as the E1001, so the server maps this to the existing mono renderer. */
#define TESSERAE_DEVICE_KIND   "xiao_epaper_75"

/* Battery sense: GPIO1 = ADC1 channel 0, 2:1 divider, gated by a load switch on
 * GPIO6 (active-high). Isolated on hardware via the ADC channel sweep: with GPIO6
 * driven high, ch0/GPIO1 reads ~2064 mV (x2 = ~4128 mV, a full 1S cell); without
 * it the pin is grounded (0 mV) -- which is why the device first reported "mains"
 * / no percentage. NOTE: the switch is GPIO6 here, NOT GPIO21 as on the
 * reTerminals (GPIO5/8/21 were ruled out one at a time). */
#define BOARD_BATTERY_ADC_CHANNEL  ADC_CHANNEL_0
#define BOARD_BATTERY_DIVIDER      2
#define BOARD_VBAT_SWITCH_PIN      6

/* MCU tier: ESP32-S3 + octal PSRAM. */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Selected panel driver: Family C, single-controller mono (shared w/ E1001). */
#define PANEL_DRIVER_MONO_SPI 1
