/*
 * Board: Waveshare "E-Paper ESP32 Driver Board" (the universal driver board,
 * SKU 15823) + a 7.5" 800x480 mono panel ("7.5inch e-Paper V2").
 *
 *   - MCU:   ESP32-WROOM-32 (original Xtensa LX6, 4 MB flash, 520 KB SRAM,
 *            NO PSRAM), USB-UART bridge (CP2102 before July 2022, CH343
 *            after), no native USB
 *   - Panel: 7.5" 800x480 mono B/W, UC8179, single controller, 1bpp
 *
 * The first classic-ESP32 target in this tree. The panel is the same glass
 * class as the E1001 / TRMNL 7.5" kit / EE04, so mono_spi carries over
 * unchanged; only the pin map and the MCU tier are new.
 *
 * Pin map is FIXED by the board (the 24-pin FPC is wired straight to the
 * module), from the Waveshare wiki "E-Paper ESP32 Driver Board", Pins table:
 * DIN=P14, SCLK=P13, CS=P15, DC=P27, RST=P26, BUSY=P25. 13/14/15 are the
 * HSPI IOMUX pins on the classic ESP32, so SPI2_HOST drives them directly.
 *
 * Two board switches matter (Waveshare wiki, "Hardware Connection"):
 *   - Switch 1 (Display Config): "B" (0.47R) for the 7.5" panel; "A" (3R)
 *     is for the small panels. Wrong position = faint or no image.
 *   - Switch 2: ON powers the USB-UART bridge. OFF saves power on battery
 *     but nothing flashes and there is no console.
 *
 * UNVERIFIED ON HARDWARE. Nobody here owns this board; the pin map is taken
 * from the vendor wiki, not observed. Flash the -selftest env first.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (single controller, SPI2 = HSPI on the classic ESP32) */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   13
#define EPD_PIN_MOSI   14
#define EPD_PIN_CS     15
#define EPD_PIN_DC     27
#define EPD_PIN_RST    26
#define EPD_PIN_BUSY   25   /* active low: 0 = busy */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. 800x480 mono, packed 1bpp = W*H/8 = 48000 bytes. */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 8)   /* 1bpp packed = 48000 */

/* Mono palette (bit 1 = white, bit 0 = black), matching mono_spi / the E1001. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1

/* Board model -> default device id "Waveshare_ESP32_Driver_75_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "Waveshare_ESP32_Driver_75"

/* Tesserae hardware-catalog kind. Frame is byte-identical to the E1001 and
 * the XIAO 7.5" boards (800x480 1bpp, 48000 bytes), so the server reuses the
 * mono renderer, but the kind stays distinct: it names the OTA lineage and
 * the signing input, and a classic-ESP32 image is not interchangeable with
 * an S3 or C3 one. */
#define TESSERAE_DEVICE_KIND   "waveshare_esp32_driver_75"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair). Same mono
 * frame as the other 800x480 1bpp boards. */
#define TESSERAE_RELAY_MODEL   "esp32_bw_client"
#define TESSERAE_RELAY_GAMUT   "mono"

/* No battery sense. The board's 5V pin accepts 3.6-5.5 V so it can run from
 * a cell, but there is no divider to any ADC pin on the board itself. Leaving
 * BOARD_BATTERY_ADC_CHANNEL undefined makes battery_read_mv() return 0, which
 * the server reads as "unknown" rather than "empty" (see battery.h). */

/* No user buttons beyond EN and BOOT, so no BOARD_BTN_REFRESH_PIN and no
 * Bluetooth setup gesture. Setup is the captive portal. */

/* MCU tier: classic ESP32, no PSRAM. Frame buffers come from internal RAM
 * (see TESSERAE_FB_CAPS in app_config.h). */
#define MCU_TIER_ESP32_NO_PSRAM 1

/* Selected panel driver: Family C, single-controller mono (shared w/ E1001). */
#define PANEL_DRIVER_MONO_SPI 1
