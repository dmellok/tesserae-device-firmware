/*
 * Board: Seeed "XIAO 7.5\" ePaper Panel" -- the INTEGRATED product (XIAO
 * ESP32-C3 + Seeed ePaper Driver Board + a 7.5" 800x480 mono panel, 1000-2000
 * mAh cell, all in one enclosure).
 *
 *   - MCU:   XIAO ESP32-C3 (RISC-V, 4 MB flash, 320 KB SRAM, NO PSRAM),
 *            native USB-Serial-JTAG (no CH340)
 *   - Panel: 7.5" 800x480 mono B/W, UC8179, single controller, 1bpp
 *
 * NOT the same board as seeed_xiao_epaper_75.h, despite the near-identical
 * name and the identical panel. That one is the S3 Plus on the EE02-style
 * *display* board (CS=44, DC=10, BUSY=4, RST=38). This one is the *driver*
 * board, which wires the panel to the XIAO's logical D-pins instead, and the
 * product ships a C3 where GPIO44 does not exist. Conflating the two is what
 * made our hardware list wrong; see the Home Assistant thread on eInk display
 * management.
 *
 * Pin map from two independent sources that agree:
 *   - Seeed_GFX User_Setups/EPaper_Board_Pins_Setups.h, the
 *     USE_XIAO_EPAPER_DRIVER_BOARD block (which the panel's own Arduino
 *     cookbook selects, along with BOARD_SCREEN_COMBO 502 = 7.5" mono UC8179)
 *   - the board schematic (ePaper_Driver_Board.pdf rev 1.0, 2024-12-09), whose
 *     connector nets read A0_D0_RST, A1_D1_CS, A2_D2_BUSY, A3_D3_DC,
 *     A8_D8_SCK, A10_D10_MOSI
 *
 * D-pin to GPIO via the XIAO ESP32-C3 Arduino variant: D0=2, D1=3, D2=4, D3=5,
 * D8=8, D10=10.
 *
 * UNVERIFIED ON HARDWARE. Nobody here owns this panel; the pin map is derived,
 * not observed. The failure mode to expect if it is wrong is a blank screen
 * with no error, because wait_busy never blocks -- exactly what happened during
 * the S3 board's bring-up.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (single controller, SPI2) -- XIAO ePaper Driver Board */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   8    /* XIAO D8  */
#define EPD_PIN_MOSI   10   /* XIAO D10 */
#define EPD_PIN_CS     3    /* XIAO D1  */
#define EPD_PIN_DC     5    /* XIAO D3  */
#define EPD_PIN_RST    2    /* XIAO D0  */
#define EPD_PIN_BUSY   4    /* XIAO D2, active low: 0 = busy */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. 800x480 mono, packed 1bpp = W*H/8 = 48000 bytes. */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 8)   /* 1bpp packed = 48000 */

/* Mono palette (bit 1 = white, bit 0 = black), matching mono_spi / the E1001. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1

/* Board model -> default device id "XIAO_ePaper_Panel_75_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "XIAO_ePaper_Panel_75"

/* Tesserae hardware-catalog kind. The frame is byte-identical to the E1001 and
 * the S3 XIAO (800x480 1bpp, 48000 bytes) so the server reuses that mono
 * renderer, but the kind must still be distinct: it names the OTA lineage and
 * the signing input file, and this board's image is not interchangeable with
 * the S3 one. */
#define TESSERAE_DEVICE_KIND   "xiao_epaper_panel_75_c3"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair). Same mono
 * frame as the other 800x480 1bpp boards. */
#define TESSERAE_RELAY_MODEL   "esp32_bw_client"
#define TESSERAE_RELAY_GAMUT   "mono"

/* NO battery sense, deliberately. The schematic charges through an ETA9740E8A
 * (U1, 0.5 A) with the cell on CN_BAT behind a mechanical slide switch, and
 * there is no divider to any ADC pin: all eleven A0-A10 nets are consumed by
 * the display interface, the charger's LED1/2/3 outputs are unconnected, and
 * H1/H2 are DNP. So the rail cannot be measured at all on this hardware.
 *
 * Leaving BOARD_BATTERY_ADC_CHANNEL undefined makes battery_read_mv() return 0,
 * which the server reads as "unknown" rather than "empty" (see battery.h).
 * Do not invent a channel here; there is nothing on the other end of it. */

/* No user buttons. The panel exposes only the XIAO's Boot and Reset buttons,
 * behind the stand and documented for entering the bootloader, so there is no
 * BOARD_BTN_REFRESH_PIN and therefore no Bluetooth setup gesture. Setup is the
 * captive portal, same as the Waveshare boards. */

/* MCU tier: ESP32-C3, single-core RISC-V, no PSRAM. Frame buffers come from
 * internal RAM (see TESSERAE_FB_CAPS in app_config.h). */
#define MCU_TIER_C3_NO_PSRAM 1

/* Selected panel driver: Family C, single-controller mono (shared w/ E1001). */
#define PANEL_DRIVER_MONO_SPI 1
