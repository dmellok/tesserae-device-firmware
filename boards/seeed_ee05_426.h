/*
 * Board: Seeed Studio XIAO ePaper Display Board EE05 + 4.26" mono panel
 *   - MCU:   XIAO ESP32-S3 Plus (ESP32-S3 + PSRAM), native USB (no CH340)
 *   - Panel: Seeed 4.26" monochrome, 800x480, SSD1677 ("SE8350 (SSD1677)" on
 *     Seeed's product page), 24-pin FPC
 *
 * Family E, reusing ssd1677_gray on its 1bpp EPD_MONO path. The controller,
 * resolution and frame are the Xteink X4's, which is verified on hardware;
 * what is new here is that panel on the EE05's pin map and power enable.
 *
 * Pin map from Seeed_GFX (User_Setups/EPaper_Board_Pins_Setups.h,
 * USE_XIAO_EPAPER_DISPLAY_BOARD_EE05), the same map boards/seeed_ee05.h uses
 * for the 2.13" BWRY panel:
 *   SCLK=D8=GPIO7, MOSI=D10=GPIO9, CS=44, DC=10, BUSY=4, RST=38, ENABLE=43.
 * Seeed_GFX drives this glass on this board with Setup506 (SSD1677_DRIVER,
 * 800x480) and the init sequence ssd1677_gray already sends.
 *
 * NOTE: CS (44) and ENABLE (43) are the ESP32-S3 UART0 RX/TX pins, so this
 * env routes the console to USB-Serial-JTAG (sdkconfig.usbjtag.defaults).
 *
 * TODO(verify): NOT YET RUN ON HARDWARE. Pins and controller are taken from
 * Seeed_GFX and compile-checked. Flash seeed-ee05-426-selftest first: its
 * wedge settles the scan direction (EPD_MIRROR_Y below).
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
#define EPD_PIN_BUSY   4    /* active HIGH on the SSD1677: 1 = busy */
#define EPD_PIN_EN     43   /* TFT_ENABLE: active-high panel power enable */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)   /* the rate the other EE05 panel runs at */

/* Frame geometry, 1bpp packed. Scan and frame agree, so ssd1677_gray skips its
 * transpose (as on the X4). */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 8)   /* 1bpp packed = 48000 */

#define EPD_MONO       1

#define EPD_PANEL_SCAN_W  800
#define EPD_PANEL_SCAN_H  480

/* The X4 drives the same controller at the same resolution and needs its gate
 * scan flipped relative to the frame; Seeed_GFX's setup for this glass sets a
 * mirror too. Start from the X4's value. TODO(verify) with the selftest wedge:
 * if the picture comes out upside down, set this to 0. Keep the server's panel
 * orientation at plain `landscape` -- correcting in both places compounds. */
#define EPD_MIRROR_Y   1

/* Mono palette (bit 1 = white), matching mono_spi / the E1001. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1

/* Board model -> default device id "Seeed_EE05_426_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "Seeed_EE05_426"
/* No TESSERAE_BLE_HARDWARE_CODE: BLE setup stays off until the panel has been
 * run on hardware and has a code in docs/ble-setup-protocol.md. Wi-Fi setup
 * goes through the captive portal. */

/* SERVER DEPENDENCY: needs the catalog entry hardware/seeed/ee05_426.json
 * (panel.gamut `mono`, renderer esp32_bw_bin). Same 48000-byte frame as the
 * E1001 / EE04 7.5" / X4, so no new renderer. Without the entry the panel
 * registers as an unknown kind. */
#define TESSERAE_DEVICE_KIND   "seeed_ee05_426"

/* Cloud-relay self-report: the device KIND that selects the .bin packer, and
 * the palette the server quantizes to. NOT the catalog id above. */
#define TESSERAE_RELAY_MODEL   "esp32_bw_client"
#define TESSERAE_RELAY_GAMUT   "mono"

/* Battery sense: GPIO1 = ADC1 channel 0, 2:1 divider, gated by a load switch
 * on GPIO6 (active-high) -- the XIAO ePaper display-board circuit shared by
 * the EE02/EE04. TODO(verify) on a real EE05. */
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

#define PANEL_DRIVER_SSD1677_GRAY 1
