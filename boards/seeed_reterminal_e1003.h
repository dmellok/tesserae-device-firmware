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

/* Battery sense (reTerminal E1003): GPIO1 = ADC1 channel 0, 2:1 divider, load
 * switch on GPIO40 (active-high). Verified on hardware via an ADC1 channel
 * sweep: ch0/GPIO1 reads ~2066 mV (x2 = ~4132 mV, a valid 1S cell); the earlier
 * ch2/GPIO3 was a saturated rail (raw 4095) that reported a bogus ~6216 mV.
 * Same sense pin/divider as the other reTerminals, contrary to the first guess. */
#define BOARD_BATTERY_ADC_CHANNEL  ADC_CHANNEL_0
#define BOARD_BATTERY_DIVIDER      2
#define BOARD_VBAT_SWITCH_PIN      40

/* Onboard SHT4x environmental sensor shared by all reTerminal E models. */
#define BOARD_HAS_SHT4X            1
#define BOARD_SHT4X_I2C_PORT       0
#define BOARD_SHT4X_I2C_SDA        19
#define BOARD_SHT4X_I2C_SCL        20
#define BOARD_SHT4X_I2C_HZ         100000
#define BOARD_SHT4X_I2C_ADDR       0x44

/* Onboard Goodix GT911 capacitive touch controller (reTerminal E1003 only).
 * Shares the SHT4x I2C bus (port 0, GPIO19/20). TP_INT on GPIO2 is RTC-capable
 * and ACTIVE-LOW (idles high, pulses low on touch -- verified on hardware); it
 * shares the buttons' ext1 ANY_LOW wake mask (ext0 did not fire on this line).
 * TP_RST on GPIO48 has an external pull-up and is NOT held across sleep (holding
 * it broke the ext1 wake). Address 0x5d is selected explicitly during reset, and
 * the point-1 coordinate block is at 0x8150 (not the usual 0x8151) -- both
 * verified on hardware (see touch_gt911.c). Enabled at runtime by the server
 * (touch_enabled config); disabled by default.
 *
 * Orientation flags map GT911 raw coords into the 1872x1404 FRAME pixel space
 * the server hit-tests against. No flips are needed: the it8951 driver's MIRROR_X
 * COMPENSATES for the ED103TC2 panel's own physical X-mirror, so the glass shows
 * the frame in normal orientation, and the GT911's raw axes already run
 * left-to-right / top-to-bottom with it. VERIFIED on hardware (e1003-selftest
 * corner taps + live touch): a top-left tap reads frame ~(0,0), so raw maps
 * straight through. (An earlier build wrongly set INVERT_X=1 to "undo" the
 * MIRROR_X and reversed every touch horizontally.) */
#define BOARD_HAS_TOUCH            1
/* Deep-sleep wake stub: bit-bang the GT911 from RTC memory ~1 ms after wake so a
 * quick tap is captured before the ~1 s boot lets the finger lift (recovers the
 * "1 in 10" misses seen on hardware). Bit-bang uses SDA=GPIO19/SCL=GPIO20 (both
 * in the low GPIO bank). Fully fail-safe -- bounded loops, always releases the
 * bus, esp_default_wake_deep_sleep() runs first. Undefine to disable. */
#define BOARD_TOUCH_WAKE_STUB      1
#define BOARD_TOUCH_INT_PIN        2
#define BOARD_TOUCH_RST_PIN        48
#define BOARD_TOUCH_I2C_ADDR       0x5d
#define BOARD_TOUCH_SWAP_XY        0
#define BOARD_TOUCH_INVERT_X       0
#define BOARD_TOUCH_INVERT_Y       0

/* Front buttons (reTerminal E baseboard). Middle "green" key on GPIO3 confirmed
 * (Seeed/TRMNL firmware wake/interrupt pin); left/right on GPIO5/GPIO4 from
 * Seeed's ESPHome reference, unverified. Active-low, RTC-capable, clear of the
 * panel pins. refresh->repaint, left->rotate prev, right->rotate next (see
 * buttons.h). GPIO3/4/5 = refresh/right/left VERIFIED on E1003 hardware directly
 * by button icon (2026-07-03); matches the E1001 baseboard. */
#define BOARD_BTN_REFRESH_PIN  3
#define BOARD_BTN_RIGHT_PIN    4
#define BOARD_BTN_LEFT_PIN     5

/* microSD (deck cache): shares the panel SPI bus with the IT8951 (SCLK 7 /
 * MOSI 9 / MISO 8 -- the same MISO the controller already reads back on)
 * with its own CS. DET low = card present. NOTE: this model's slot power
 * gate is GPIO39, not 16 like the other reTerminals (Seeed cookbook table). */
#define TESSERAE_SD_SLOT   1
#define SD_SPI_SHARED_BUS  1
#define SD_PIN_MISO   8
#define SD_PIN_CS     14
#define SD_PIN_DET    15
#define SD_PIN_EN     39

/* Board model -> default device id "reTerminal_E1003_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "reTerminal_E1003"

/* Tesserae hardware-catalog kind. NOTE: the E1003 needs a 4bpp GRAYSCALE
 * (16-level, 1872x1404) server renderer, distinct from the Spectra / mono
 * paths. Confirm this id and that a matching manifest exists in Tesserae. */
#define TESSERAE_DEVICE_KIND   "seeed_reterminal_e1003"

/* MCU tier: ESP32-S3 + PSRAM (assumed octal; verify on hardware). */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Local overlay render mode (overlay.h): the IT8951 has usable partial
 * refresh (DU waveform for echo rects, GC16 for hygiene), so this board
 * advertises "overlay": {"schema": 1} and runs tap echo / value slots
 * locally. Spectra boards never define this (no useful partial refresh). */
#define BOARD_OVERLAY_PARTIAL 1

/* Selected panel driver: Family D, IT8951 grayscale over SPI. */
#define PANEL_DRIVER_IT8951_GRAY 1
