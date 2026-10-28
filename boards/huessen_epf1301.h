/*
 * Board: Huessen 13.3" WiFi picture frame (EPF1301)
 *   - MCU:   ESP32-S3 (QFN56, rev v0.2), 16 MB flash, 8 MB octal PSRAM,
 *            native USB-Serial-JTAG only (no UART bridge)
 *   - Panel: E Ink EL133UF1 13.3" Spectra 6, dual-chip, 1200x1600 portrait
 *            memory, mounted landscape in the frame
 *   - Power: Li-ion cell; the panel runs off the battery rail, so it will not
 *            paint on USB power alone
 *
 * The pin map and power sequence follow the stock E_Frame firmware.
 * The carrier looks derived from Good Display's ESP32-133C02 reference
 * (SCLK 9 / MOSI 41 / CS0 18 / RST 6 / BUSY 7 match), but there is no DC line,
 * the second chip-select is on GPIO8, and GPIO17 switches the display rail,
 * so it runs its own driver (huessen_epf1301_dual) -- see that file.
 *
 * Confirmed on hardware: selftest colour bars (palette and both controller
 * halves), the captive portal, registration, and frames from the server.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (SPI2)                                                */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   9
#define EPD_PIN_MOSI   41
#define EPD_PIN_CS_M   18   /* CTRL1: left half (cols 0..599), low = selected   */
#define EPD_PIN_CS_S   8    /* CTRL2: right half (cols 600..1199)               */
#define EPD_PIN_RST    6    /* active low                                       */
#define EPD_PIN_BUSY   7    /* active low; external pull-up, internal must stay off */
#define EPD_PIN_PWR    17   /* SYS_POWER: active-high display rail              */
/* No DC line: the controllers are strapped 3-wire and frame opcodes by CS.
 * GPIO0 is the vendor's (unconnected) SPI hardware CS and a boot strap; this
 * firmware never drives it. */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (8 * 1000 * 1000)   /* the vendor's clock */

/* Panel geometry. Portrait-native 1200x1600, 4bpp packed dual-controller --
 * the same 960000-byte frame as the E1004 / Inkplate 13SPECTRA. */
#define EPD_WIDTH      1200
#define EPD_HEIGHT     1600
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)

/* The frame hangs landscape, but this firmware paints the portrait buffer
 * as-is and turns nothing on the device. Register with rotation 0 so the
 * server packs frames at the 1200x1600 stride and turns a landscape dashboard
 * onto it, whichever way the kind's catalog entry has the panel. */
#define BOARD_FRAMEBUFFER_ROTATION 0

/* 6-colour Spectra palette indices (nibble values), confirmed on this glass. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3
#define EPD_COL_BLUE    0x5
#define EPD_COL_GREEN   0x6

#define TESSERAE_DEVICE_MODEL  "Huessen_EPF1301"
/* The server needs a hardware-catalog entry for this kind: esp32_client
 * protocol, esp32_bin renderer, raw 4bpp Spectra-6, 1200x1600, 960000 bytes
 * (the seeed_reterminal_e1004 format exactly). */
#define TESSERAE_DEVICE_KIND   "huessen_epf1301"
/* No TESSERAE_BLE_HARDWARE_CODE: BLE setup stays off until Companion has a
 * code for this kind (docs/ble-setup-protocol.md). Setup is the captive portal
 * or serial over USB. */

#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "spectra_6"

/* ------------------------------------------------------------------ */
/* Power rails and enables                                             */
/* ------------------------------------------------------------------ */
/* GPIO3: the stock firmware drives it high at boot and never low while awake;
 * the battery-sense front end needs it.
 * GPIO4 / GPIO13: charger enables, active LOW; the stock firmware drives both
 * low at boot. GPIO2: red "work" LED, active high, kept off.
 *
 * Asleep: GPIO3 parks low, the charger enables stay low (charging stays on
 * through sleep), the red LED stays off, and the display rail (GPIO17) is
 * held off so a floating enable cannot power the panel for the whole sleep.
 * The panel driver releases GPIO17's hold at port init. */
#define BOARD_WAKE_DRIVE_HIGH_MASK  (1ULL << 3)
#define BOARD_WAKE_DRIVE_LOW_MASK   ((1ULL << 2) | (1ULL << 4) | (1ULL << 13))
#define BOARD_SLEEP_DRIVE_LOW_MASK  ((1ULL << 2) | (1ULL << 3) | (1ULL << 4) | \
                                     (1ULL << 13) | (1ULL << EPD_PIN_PWR))

/* Battery: GPIO5 = ADC1 channel 4, through a 3.34:1 divider (pin ~1230 mV at
 * a 4.1 V cell). */
#define BOARD_BATTERY_ADC_CHANNEL   ADC_CHANNEL_4
#define BOARD_BATTERY_DIVIDER_X100  334

/* WiFi LED on GPIO38, active high (the stock firmware fades it on LEDC; plain
 * on/off is enough for the status pattern). */
#define BOARD_LED_PIN               38
#define BOARD_LED_ACTIVE_LOW        0

/* Buttons. GPIO1 is the only usable wake key: active low, the stock
 * firmware's "power off" key, and it drives the refresh action here. The other
 * keys are not used: GPIO12 (power) toggles in lockstep with the USB-host
 * detect on GPIO14, so it reads as a press on every cable plug, and GPIO39 /
 * GPIO40 are not RTC GPIOs, so they cannot join the ext1 wake mask. */
#define BOARD_BTN_REFRESH_PIN  1

/* ------------------------------------------------------------------ */
/* MCU tier and panel driver                                           */
/* ------------------------------------------------------------------ */
#define MCU_TIER_S3_OCTAL_PSRAM 1
#define PANEL_DRIVER_HUESSEN_EPF1301_DUAL 1
