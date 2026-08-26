/*
 * Board: Xteink X4
 *   - MCU:   ESP32-C3 (RISC-V, 16 MB flash, NO PSRAM), native USB-Serial-JTAG
 *   - Panel: 4.26" 800x480 mono, SSD1677 (same controller as the Sticky)
 *
 * Family E, reusing ssd1677_gray. Differs from the Sticky in its pin map, in
 * not being mounted rotated (so no transpose), in running the driver's 1bpp
 * EPD_MONO path, and in needing EPD_MIRROR_Y.
 *
 * Pins/controller/SPI clock transcribed from the MIT-licensed FreeInk SDK's
 * XTEINK_X4 board profile, which drives this hardware in CrossInk.
 *
 * BATCH VARIATION: later X4 runs ship a UC8179 or UC8279 instead of the
 * SSD1677 on the same board and glass. This is the SSD1677 build; such a unit
 * will not refresh at all and wants a target of its own.
 */
#pragma once

/* Panel pin map (single controller, SPI2). SCLK/MOSI shared with the SD slot. */
#define EPD_PIN_SCLK   8
#define EPD_PIN_MOSI   10
#define EPD_PIN_CS     21
#define EPD_PIN_DC     4
#define EPD_PIN_RST    5
#define EPD_PIN_BUSY   6    /* active HIGH: 1 = busy */
/* No EPD_PIN_EN: the panel rail is not gated on this board. */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (20 * 1000 * 1000)   /* SSD1677 datasheet max for write */

/* Frame geometry, 1bpp packed. Scan and frame agree -- the glass is mounted in
 * the controller's native orientation, so ssd1677_gray skips its transpose. */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 8)   /* 1bpp packed = 48000 */

/* MONO, not the 4-gray this controller also does. The glass handles grey (the
 * selftest confirmed it), but mono halves the frame to 48000 bytes -- the same
 * size the proven XIAO C3 board carries, which matters on a part with no PSRAM
 * running WiFi and TLS -- and matches how Tesserae had the device registered.
 * To switch: drop EPD_MONO, use /4 above, restore the two mid-grey palette
 * entries, set gamut gray_4 here and on the server. */
#define EPD_MONO       1

#define EPD_PANEL_SCAN_W  800
#define EPD_PANEL_SCAN_H  480

/* The gate scan runs bottom-to-top relative to the frame. Confirmed with the
 * selftest wedge, and matching FreeInk, which drives this controller with
 * data-entry 0x11 = 0x01 (Y decrement) where the Sticky path uses 0x03.
 * The Sticky needs no mirror because its 90-degree transpose already fixes
 * each axis; the X4 streams straight through and exposes the scan direction.
 * Keep the server's panel orientation at plain `landscape` -- correcting in
 * both places compounds. */
#define EPD_MIRROR_Y   1

/* Mono palette (bit 1 = white), matching mono_spi / the E1001. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1

#define TESSERAE_DEVICE_MODEL  "Xteink_X4"

/* SERVER DEPENDENCY: needs a catalog entry for xteink_x4 with panel.gamut
 * `mono` and the esp32_bw_bin renderer -- same 48000-byte frame as the E1001 /
 * TRMNL mono boards, so that renderer is reused as-is. Without it the panel
 * pairs but every frame is rejected on length. */
#define TESSERAE_DEVICE_KIND   "xteink_x4"

#define TESSERAE_RELAY_MODEL   "esp32_bw_client"
#define TESSERAE_RELAY_GAMUT   "mono"

/* Battery: plain divider to GPIO0 (ADC1 ch0), halving the cell. No gauge and no
 * charge-status line on this board (the X3 has both; the X4 has neither). */
#define BOARD_BATTERY_ADC_CHANNEL  ADC_CHANNEL_0
#define BOARD_BATTERY_DIVIDER      2

/* The power button is the only real GPIO key, and it is inside GPIO0-5, the
 * pads the C3 can wake from deep sleep on. The six nav keys are not GPIOs at
 * all -- they sit on ADC resistor ladders (GPIO1, GPIO2) read by voltage band,
 * which buttons.h has no backend for. */
#define BOARD_BTN_REFRESH_PIN  3    /* power button, active-low */

/* ...reported as "right", not "refresh". The names are conventions the server
 * maps to actions, and Tesserae's built-in map binds "right" to rotate_next
 * while "refresh" only re-renders the step already up. On a one-key panel
 * showing a rotation, advance is the useful gesture -- and this way a stock
 * server needs no per-device button map. Set to "refresh" to get a re-render
 * instead. Confirmed: a press logs `trigger=right spec=rotate_next -> step=1`. */
#define BOARD_BTN_REFRESH_WIRE_NAME  "right"

/* GPIO13 gates the battery MOSFET. Most units self-latch through a pull once
 * the button bridges the rail, but at least one field revision does not and
 * stays up only while the button is held. Asserting is a no-op on the former.
 * Not a bus pin here; on the X4 Pro GPIO13 is the display CS. */
#define BOARD_POWER_LATCH_PIN  13

/* microSD shares the panel bus with its own CS; MISO is SD-only. Not power
 * gated on the X4 (the X3 gates it on GPIO13, which the X4 spends on the latch). */
#define TESSERAE_SD_SLOT   1
#define SD_SPI_SHARED_BUS  1
#define SD_PIN_MISO   7
#define SD_PIN_CS     12

/* Known but unused: GPIO20 is a USB-attach sense line. The sleep path already
 * asks usb_serial_jtag_is_connected(), which detects a USB data host. */

/* MCU tier: ESP32-C3, no PSRAM -- frame buffers come from internal RAM. */
#define MCU_TIER_C3_NO_PSRAM 1

#define PANEL_DRIVER_SSD1677_GRAY 1
