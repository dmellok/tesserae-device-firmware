/*
 * Board: Seeed reTerminal E1003
 *   - MCU:   XIAO ESP32-S3 class (ESP32-S3 + PSRAM)
 *   - Panel: 10.3" ED103TC2 grayscale, 1872x1404, via an IT8951 controller
 *
 * Family D. Uses the it8951_gray driver. The ESP32 talks to an IT8951 timing
 * controller over SPI (bidirectional -- it reads device info to learn the
 * controller's image-buffer address); the IT8951 drives the panel's parallel
 * glass internally. Driven in 4bpp (16-level) grayscale with the GC16 waveform.
 *
 * Reference: bitbank2 FastEPD (initIT8951 + setPanelSize(BBEP_DISPLAY_ED103TC2)).
 * Pins from the reTerminal E1003 pin map.
 *
 * There is NO DC pin (command vs data is encoded in a 16-bit SPI preamble), and
 * two power-enable lines (EN + VCC_EN). VERIFY the server-side renderer: the
 * frame must be 4bpp packed grayscale (1314144 bytes) for TESSERAE_DEVICE_KIND.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* IT8951 SPI pin map (XIAO ESP32-S3 GPIO numbers)                     */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK    7
#define EPD_PIN_MOSI    9
#define EPD_PIN_MISO    8    /* IT8951 is bidirectional (device-info reads) */
#define EPD_PIN_CS      10
#define EPD_PIN_RST     12
#define EPD_PIN_BUSY    13   /* HRDY: HIGH = ready (opposite of the UC81xx panels) */
#define EPD_PIN_EN      11   /* panel power enable */
#define EPD_PIN_VCC_EN  21   /* controller (ITE) power enable */

#define EPD_SPI_HOST    SPI2_HOST
#define EPD_SPI_HZ      (4 * 1000 * 1000)   /* FastEPD run freq; reads need a modest clock */

/* Panel geometry. 1872x1404, 4bpp packed grayscale = 2 px/byte. */
#define EPD_WIDTH       1872
#define EPD_HEIGHT      1404
#define EPD_BUF_BYTES   ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 4bpp packed = 1314144 */

/* Grayscale levels (nibble values): 0x0 = black ... 0xF = white. The shared
 * 4bpp splash path reuses these (white nibble 0xF instead of the Spectra 0x1). */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0xF

/* IT8951 VCOM, in mV (magnitude of the negative VCOM). FastEPD uses 1500
 * (-1.5 V) as a safe value for large panels; the ED103TC2's rated VCOM may
 * differ slightly (tune if contrast is off). */
#define EPD_VCOM_MV     1500

/* Battery sense (reTerminal E1003): GPIO3 = ADC1 channel 2, 2:1 divider,
 * load switch on GPIO40 (active-high). NOTE: different pins from E1001/2/4. */
#define BOARD_BATTERY_ADC_CHANNEL  ADC_CHANNEL_2
#define BOARD_BATTERY_DIVIDER      2
#define BOARD_VBAT_SWITCH_PIN      40

/* Board model -> default device id "reTerminal_E1003_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "reTerminal_E1003"

/* Tesserae hardware-catalog kind. NOTE: the E1003 needs a 4bpp GRAYSCALE
 * (16-level, 1872x1404) server renderer, distinct from the Spectra / mono
 * paths. Confirm this id and that a matching manifest exists in Tesserae. */
#define TESSERAE_DEVICE_KIND   "seeed_reterminal_e1003"

/* MCU tier: ESP32-S3 + PSRAM (assumed octal; verify on hardware). */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Selected panel driver: Family D, IT8951 grayscale over SPI. */
#define PANEL_DRIVER_IT8951_GRAY 1
