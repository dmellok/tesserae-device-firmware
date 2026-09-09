/*
 * Board: paperlesspaper OpenPaper L (13.3" Spectra 6 e-paper picture frame)
 *   - MCU:   ESP32-C6-MINI-1-N4 (RISC-V, 4 MB flash, ~512 KB SRAM, NO PSRAM),
 *            CP2102N USB-UART bridge on UART0
 *   - Panel: 13.3" 1200x1600 Spectra 6 (E Ink EL133UF3), dual controller
 *            (CS_M / CS_S), 4bpp packed = 960000 bytes
 *   - Power: 4 x AA NiMH behind a BQ25172 charger
 *
 * BUILT BLIND. Nobody here has the L; the pin map and the panel command
 * framing come from the vendor firmware's EPD_TYPE_13INCH build
 * (paperlesspaper/paperlesspaper-firmware, src/main.cpp + src/epaper_display.*
 * and their GxEPD2 fork's GxEPD2_1330c_EL133UF3). Flash the -selftest env first.
 *
 * Two things make this board unlike every other Spectra-6 dual target:
 *
 *   1. NO DC LINE. The vendor wires the two chip-selects to GPIO20/19 and
 *      constructs the panel driver with DC=-1: every command is one CS-framed
 *      SPI burst whose first byte is the opcode and the rest its parameters.
 *      EPD_NO_DC below makes spectra6_spi_dual skip the DC GPIO; the transfer
 *      framing it already uses is exactly that.
 *
 *   2. NO ROOM FOR THE FRAME. 960000 bytes cannot live in the C6's SRAM, so
 *      TESSERAE_STREAM_FRAMES routes the frame download straight into the
 *      panel controllers' RAM in row blocks through partial-window writes
 *      (PTLW 0x83 / PTIN 0x91 / DTM 0x10), the way the vendor streams it from
 *      their staging flash. The splash renders in the same row bands. See
 *      epd_panel.h stream_* and image_fetch_to_sink().
 *
 * The register init block is byte-identical to the Waveshare 13.3E6 one the
 * dual driver already carries (the vendor's GxEPD2 fork lists the same PSR /
 * PWR / CDI / TCON / TRES / AN_TM / AGID / BTST values), so the driver is
 * shared and only the framing and transport differ.
 *
 * Buttons: big = EN (hardware reset = wake and repaint), small = BOOT. No
 * ext1 buttons, no BLE gesture; setup is the captive portal.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (ESP32-C6 GPIO numbers)                                */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   15
#define EPD_PIN_MOSI   4
#define EPD_PIN_CS_M   20   /* left  half (cols   0..599), "CS_EPD_PIN"  */
#define EPD_PIN_CS_S   19   /* right half (cols 600..1199), "EPD_CS_S"   */
#define EPD_PIN_RST    1
#define EPD_PIN_BUSY   18
#define EPD_PIN_PWR    12   /* active-high panel rail ("DISP_POWER")       */
#define EPD_NO_DC      1    /* no data/command line: CS-framed opcodes    */
#define EPD_PWR_SETTLE_MS 100   /* vendor waits 100 ms after the rail     */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. Native orientation is portrait. Same frame layout as the
 * Waveshare 13.3E6: 1600 rows of 600 packed bytes, first 300 to CS_M. */
#define EPD_WIDTH      1200
#define EPD_HEIGHT     1600
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 4bpp packed = 960000 */

/* 6-colour palette indices (nibble values). 4 and 7 are reserved. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3
#define EPD_COL_BLUE    0x5
#define EPD_COL_GREEN   0x6

/* Stream the frame to the panel in blocks of this many rows (60000 bytes of
 * SRAM at 600 bytes/row). Must be even: the panel's window registers count
 * row pairs. The vendor streams 16 blocks of 100. */
#define TESSERAE_STREAM_FRAMES 1
#define EPD_STREAM_ROWS        100

/* Board model -> default device id "OpenPaper_L_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "OpenPaper_L"

/* Tesserae hardware-catalog kind (esp32_client protocol + esp32_bin renderer:
 * portrait 1200x1600 4bpp, 960000 bytes; hardware/paperlesspaper/openpaper_l.json). */
#define TESSERAE_DEVICE_KIND   "paperlesspaper_openpaper_l"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair). */
#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "waveshare_e6"

/* Battery sense: same ADC pin as the 7 (GPIO2 = ADC1 channel 2) but the
 * vendor's 13" build drives the sense enable HIGH and scales raw counts by
 * 6.68, about 8.3:1 against a 3.3 V full scale. No schematic is published
 * for the L board, so this is transcribed, not derived. Until a meter has
 * confirmed it, the low-battery goodbye and the OTA battery floor are
 * disabled (thresholds 0) so a mis-scaled reading cannot hibernate a fresh
 * unit; the heartbeat still reports whatever it reads. */
#define BOARD_BATTERY_ADC_CHANNEL     ADC_CHANNEL_2
#define BOARD_BATTERY_DIVIDER_X100    829
#define BOARD_VBAT_SWITCH_PIN         3
#define BOARD_BATTERY_NIMH_CELLS      4
#define BATTERY_GOODBYE_MV            0
#define BATTERY_RESUME_MV             0
#define TESSERAE_OTA_MIN_BATTERY_MV   0

/* CP2102N on the USB-C port; the C6's USB-Serial-JTAG pads are GPIO12/13,
 * and GPIO12 is the panel rail above. sdkconfig.c6.defaults turns the USB
 * PHY off so the pin is a plain GPIO. */
#define BOARD_USB_SERIAL_JTAG_ABSENT  1

/* A full Spectra-6 refresh on this glass is ~25-30 s; the vendor expects
 * >= 10 s and treats faster as a failed paint. */
#define AWAKE_MIN_REPAINT_S 60

/* MCU tier: ESP32-C6, no PSRAM. Frames never exist in RAM on this board. */
#define MCU_TIER_C6_NO_PSRAM 1

/* Selected panel driver: Family A, dual-controller Spectra-6, with the
 * no-DC framing and the streaming entry points enabled by the macros above. */
#define PANEL_DRIVER_SPECTRA6_SPI_DUAL 1
