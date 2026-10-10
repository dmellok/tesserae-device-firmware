/*
 * Board: Waveshare 7.3" e-Paper (F) on a generic ESP32-S3 dev board
 *   - MCU:   ESP32-S3 with 16 MB flash and 8 MB octal PSRAM (N16R8, as on the
 *            DevKitC-1); other modules need their own flash/PSRAM settings)
 *   - Panel: 7.3" 800x480 7-colour ACeP (AC073TC1A), single controller, SPI,
 *            wired by hand or through Waveshare's e-Paper Driver HAT
 *
 * Family B. Reuses spectra6_spi_single (same UC81xx command set and SPI
 * transport) with EPD_INIT_ACEP_7IN3F, which swaps in the 7in3f init block.
 * The frame is 192000 bytes of 4bpp ACeP indices in the panel's own order,
 * which is the server's inky_7colour packing, so nothing is remapped.
 *
 * There is no one board for this glass: people wire it to whatever S3 they
 * have. The pins below are a default for an ESP32-S3-DevKitC-1 (GPIOs clear
 * of the strapping pins, USB and the octal PSRAM bus); each can be overridden
 * from platformio.ini build_flags, e.g. -DEPD_PIN_BUSY=4. No battery, no
 * buttons, no sensors. UNVERIFIED ON HARDWARE.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (single chip-select, SPI2). Override any of these.    */
/* ------------------------------------------------------------------ */
#ifndef EPD_PIN_SCLK
#define EPD_PIN_SCLK   12
#endif
#ifndef EPD_PIN_MOSI
#define EPD_PIN_MOSI   11
#endif
#ifndef EPD_PIN_CS
#define EPD_PIN_CS     10
#endif
#ifndef EPD_PIN_DC
#define EPD_PIN_DC     9
#endif
#ifndef EPD_PIN_RST
#define EPD_PIN_RST    8
#endif
#ifndef EPD_PIN_BUSY
#define EPD_PIN_BUSY   7    /* active low: 0 = busy */
#endif

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. Landscape-native 800x480, 4bpp packed = 192000 bytes. */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)

/* Swaps in the ACeP init block in spectra6_spi_single. */
#define EPD_INIT_ACEP_7IN3F 1

/* 7-colour ACeP indices (nibble values), the panel's native order. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_GREEN   0x2
#define EPD_COL_BLUE    0x3
#define EPD_COL_RED     0x4
#define EPD_COL_YELLOW  0x5
#define EPD_COL_ORANGE  0x6

/* Board model -> default device id "Waveshare_73F_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "Waveshare_73F"

/* Tesserae hardware-catalog kind (hardware/waveshare/waveshare_epaper_73f.json). */
#define TESSERAE_DEVICE_KIND   "waveshare_epaper_73f"

/* Cloud-relay self-report: the esp32_client packer with the ACeP palette in
 * the panel's order (inky_7colour; acep_7colour is its alias). */
#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "inky_7colour"

/* MCU tier: ESP32-S3 + octal PSRAM. */
#define MCU_TIER_S3_OCTAL_PSRAM 1

#define PANEL_DRIVER_SPECTRA6_SPI_SINGLE 1
