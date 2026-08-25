/*
 * Board: M5Stack PaperS3 (SKU C139)
 *   - MCU:   ESP32-S3R8 (8 MB octal PSRAM, 16 MB flash)
 *   - Panel: 4.7" ED047TC1, 960x540, 16-level greyscale, RAW PARALLEL glass
 *
 * Family F. Uses the parallel_epd_gray driver -- the first target here with no
 * panel controller at all. The ESP32-S3 drives the source driver directly over
 * its 8-bit LCD/i80 bus and walks the gate driver on SPV/CKV, so the "init
 * sequence" is a power ramp and the waveform is the grey matrix below.
 *
 * Pin map, panel constant and grey matrix from bitbank2 FastEPD's
 * BB_PANEL_M5PAPERS3, cross-checked against the ESPHome ed047tc1 component the
 * reporter had running (patrick3399/esphome_components) and M5's own docs.
 * Both sources agree pin for pin.
 *
 * UNVERIFIED ON HARDWARE. Flash m5stack-papers3-selftest first and judge the
 * 16 grey bands; see the notes on orientation and touch below for the two
 * things a first bring-up is most likely to land on.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Parallel panel bus (ESP32-S3 LCD/i80 peripheral)                    */
/* ------------------------------------------------------------------ */
/* D0..D7. Order matters: this is the bit order the source driver sees. */
#define EPD_PAR_DATA_PINS   {6, 14, 7, 12, 9, 11, 8, 10}
#define EPD_PAR_BUS_WIDTH   8
#define EPD_PAR_PCLK_HZ     (40 * 1000 * 1000)

#define EPD_PIN_CL          16   /* pixel clock; the i80 bus's WR line */
#define EPD_PIN_SPH         13   /* start pulse horizontal; the i80 bus's CS */
#define EPD_PIN_LE          15   /* latch shifted row into the driver outputs */
#define EPD_PIN_SPV         17   /* start pulse vertical (gate driver token) */
#define EPD_PIN_CKV         18   /* gate driver clock; one pulse = one row */

/* Two supply enables, raised in this order and dropped in reverse. FastEPD
 * calls them ioOE and ioPWR, the ESPHome component calls them pwr and
 * bst_enable; they are the same two lines, named here for what they do. */
#define EPD_PIN_PWR         45   /* panel rail            (ESPHome: pwr)        */
#define EPD_PIN_BST_EN      46   /* +/-15 V boost         (ESPHome: bst_enable) */

/* The i80 driver requires a D/C pin this panel has no use for. GPIO47 is
 * unrouted on this board, so it absorbs the signal. */
#define EPD_PIN_DC_DUMMY    47

/* Trailing clocks per row: the source driver's shift register runs past the
 * visible glass, and without these the rightmost pixels never latch. */
#define EPD_PAR_LINE_PADDING 16

/* Panel geometry, in the controller's own scan order: 960 across, 540 down.
 * 4bpp packed greyscale = 2 px/byte, so 259200 bytes a frame. The device is
 * PORTRAIT in the hand (121.5 x 67.7 mm body, 4.7" 16:9 glass -- the 960 axis
 * runs the long way), so the server composes 540x960 and the renderer rotates
 * into this stride. See the catalog entry's notes_md. */
#define EPD_WIDTH           960
#define EPD_HEIGHT          540
#define EPD_BUF_BYTES       ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 259200 */

/* Grey nibbles: 0x0 = black ... 0xF = white, matching the shared 4bpp splash
 * path and the server's esp32_gray_bin wire format. */
#define EPD_COL_BLACK       0x0
#define EPD_COL_WHITE       0xF

#define EPD_PAR_PANEL_NAME  "Parallel EPD greyscale 4.7\" (960x540, 4bpp)"

/* Greyscale pass matrix: 16 rows (grey level 0 = black .. 15 = white) x 8
 * columns (one per drive pass). Each entry is a 2-bit drive code -- 1 darken,
 * 2 lighten, 0 neutral -- so level 0 is eight darkens and level 15 eight
 * lightens. Assumes the panel starts from white, which the driver's clear
 * cycle guarantees.
 *
 * This is FastEPD's u8M5Matrix, measured on this glass at 20 C. It is the one
 * table to reach for if the ramp comes out uneven: an entry that is 1 where it
 * should be 2 shows up as two adjacent bands collapsing into one tone. */
#define EPD_PAR_GRAY_MATRIX { \
    /*  0 */ 1, 1, 1, 1, 1, 1, 1, 1, \
    /*  1 */ 2, 2, 1, 1, 2, 1, 1, 1, \
    /*  2 */ 2, 2, 1, 1, 1, 1, 2, 1, \
    /*  3 */ 2, 2, 1, 1, 2, 2, 1, 1, \
    /*  4 */ 2, 2, 2, 2, 1, 1, 2, 1, \
    /*  5 */ 2, 2, 1, 1, 1, 2, 2, 1, \
    /*  6 */ 2, 2, 1, 1, 2, 1, 1, 2, \
    /*  7 */ 2, 2, 2, 1, 2, 1, 1, 2, \
    /*  8 */ 2, 2, 2, 2, 2, 1, 2, 1, \
    /*  9 */ 1, 1, 1, 1, 1, 1, 2, 2, \
    /* 10 */ 2, 2, 1, 1, 1, 1, 2, 2, \
    /* 11 */ 1, 1, 1, 1, 2, 1, 2, 2, \
    /* 12 */ 2, 2, 1, 1, 2, 1, 2, 2, \
    /* 13 */ 2, 1, 1, 2, 2, 1, 2, 2, \
    /* 14 */ 2, 2, 1, 2, 2, 1, 2, 2, \
    /* 15 */ 2, 2, 2, 2, 2, 2, 2, 2 }

/* Candidate retune, round 2 (issue #21), painted by the selftest on the
 * bottom half of the lettered ramp sheet (top half = shipped matrix) and NOT
 * used for normal display until the reporter's letter ordering confirms it.
 *
 * Round 1's model-derived candidate fixed the level-9 collapse but overshot
 * level 6 badly and pinched 7..9 into one tone: its single code flips
 * realised 2-4x the shift the linear pass-weight model predicted, and
 * inconsistently between rows, so the glass response is state-dependent and
 * extrapolating new pass patterns is not trustworthy. This table therefore
 * invents no patterns at all. The two rounds of photos provide measured
 * tones for 22 distinct patterns on this unit; each level below is the
 * already-measured pattern whose tone lands closest to that level's target,
 * reassigned so the ramp is monotone in MEASURED tone through level 14.
 * Known residuals: no measured pattern falls between levels 7 and 8 (that
 * step stays a little wide), and 14/15 stay nearly merged (on this glass
 * eight straight lightens measures no lighter than the mixed level-13/14
 * patterns). Full derivation: notes/papers3-gray-matrix-tuning.md. */
#define EPD_PAR_GRAY_MATRIX_B { \
    /*  0 */ 1, 1, 1, 1, 1, 1, 1, 1, \
    /*  1 */ 2, 2, 1, 1, 2, 1, 1, 1, \
    /*  2 */ 2, 2, 1, 1, 1, 1, 2, 1, \
    /*  3 */ 2, 2, 2, 1, 1, 1, 2, 1, \
    /*  4 */ 1, 1, 1, 1, 1, 1, 2, 2, \
    /*  5 */ 2, 2, 1, 1, 2, 2, 1, 1, \
    /*  6 */ 2, 2, 1, 1, 2, 1, 1, 2, \
    /*  7 */ 2, 2, 1, 1, 1, 1, 2, 2, \
    /*  8 */ 2, 2, 2, 2, 1, 1, 2, 1, \
    /*  9 */ 2, 1, 2, 1, 1, 1, 2, 2, \
    /* 10 */ 2, 2, 2, 2, 2, 1, 2, 1, \
    /* 11 */ 1, 1, 1, 1, 2, 1, 2, 2, \
    /* 12 */ 2, 2, 1, 2, 1, 1, 2, 2, \
    /* 13 */ 2, 2, 1, 1, 2, 1, 2, 2, \
    /* 14 */ 2, 1, 1, 2, 2, 1, 2, 2, \
    /* 15 */ 2, 2, 2, 2, 2, 2, 2, 2 }

/* VCOM is fixed in this board's supply (-1.6 V per FastEPD's panel def); there
 * is no digital PMIC to program, so this is documentation, not a setting. */
#define EPD_VCOM_MV         1600

/* ------------------------------------------------------------------ */
/* Battery                                                             */
/* ------------------------------------------------------------------ */
/* GPIO3 = ADC1 channel 2, 2:1 divider, feeding a 1800 mAh cell. No load
 * switch on this board -- the divider is always connected. UNVERIFIED: if the
 * reported voltage reads about double a plausible cell, the divider is 1:1.
 *
 * The board also exposes charge status on GPIO4 (active low) and USB-power
 * detect on GPIO5 (2:1 divider). Neither has a consumer in this firmware yet;
 * they are recorded here so the next person does not have to re-derive them. */
#define BOARD_BATTERY_ADC_CHANNEL  ADC_CHANNEL_2
#define BOARD_BATTERY_DIVIDER      2

/* ------------------------------------------------------------------ */
/* Touch -- present, NOT enabled yet                                   */
/* ------------------------------------------------------------------ */
/* A Goodix GT911 sits on the shared I2C bus (SDA 41 / SCL 42) with its
 * interrupt on GPIO48. Deliberately not switched on: this board gives the MCU
 * no TP_RST line, and touch_gt911.c drives the reset + address-select sequence
 * on BOARD_TOUCH_RST_PIN to pin the controller to 0x5d. Enabling touch here
 * means first teaching that driver a reset-less path (probe 0x5d, then 0x14,
 * skip the reset), which is its own change and its own bring-up.
 *
 * The panel comes first regardless: a display that paints correctly is the
 * prerequisite for any touch result being meaningful. The pins are recorded so
 * that work starts from facts.
 *
 *   TP INT   GPIO48        I2C SDA  GPIO41      I2C SCL  GPIO42 @ 200 kHz
 *   TP RST   not routed    address  0x5d (strapped)
 *
 * The ESPHome config the reporter had running sets mirror_x + mirror_y on the
 * touch transform, so expect BOARD_TOUCH_INVERT_X/Y = 1 as the starting guess.
 */

/* Shared I2C bus: BMI270 gyro at 0x68, BM8563 RTC at 0x51, GT911 touch.
 * Declared without a sensor consumer -- this board has no SHT4x/SHTC3 -- so
 * nothing reads it yet. */
#define BOARD_PAPERS3_I2C_SDA      41
#define BOARD_PAPERS3_I2C_SCL      42
#define BOARD_PAPERS3_I2C_HZ       200000

/* ------------------------------------------------------------------ */
/* Buttons                                                             */
/* ------------------------------------------------------------------ */
/* One physical button only, and it is not the MCU's: the side key runs to a
 * PMS150G that owns power on/off and download mode. There is no GPIO the
 * firmware can read as Refresh/Left/Right, so this board ships without the
 * front-button feature (no BOARD_BTN_* -- buttons.h gates on those being
 * defined). Rotation and refresh come from the server. */

/* Board model -> default device id "M5PaperS3_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "M5PaperS3"

/* Tesserae hardware-catalog kind. Selects the server-side renderer and .bin
 * packer: esp32_gray_bin, the same 4bpp linear-greyscale wire format the E1003
 * uses, at this panel's geometry. A kind is also the OTA lineage, so it is
 * fixed from first release. See hardware/m5stack/papers3.json. */
#define TESSERAE_DEVICE_KIND   "m5stack_papers3"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair). "protocol"
 * is the device kind's transport family, "gamut" the palette the server
 * quantizes to -- both hardware facts, so the panel reports them at pairing.
 * NOTE the relay model is NOT the catalog kind above. */
#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "gray_16"

/* MCU tier: ESP32-S3 + 8 MB octal PSRAM (M5 spec sheet). */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Battery-powered handheld, so no BOARD_MAINS_POWERED: always-on is not on
 * offer for a 1800 mAh cell. */

/* Selected panel driver: Family F, raw parallel glass in 16-level grey. */
#define PANEL_DRIVER_PARALLEL_EPD_GRAY 1
