/*
 * Board: M5Stack PaperDIY (Complete Kit K159 / Assembly Kit)
 *   - MCU:   ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM, native USB)
 *   - Panel: 4.7" ED047TC1, 960x540, 16-level greyscale, RAW PARALLEL glass
 *   - Power: M5PM1 PMIC at 0x6E (panel rail, battery, power button)
 *
 * Family F, the PaperS3's parallel_epd_gray driver on the same glass. The
 * whole bus is pin-identical to boards/m5stack_papers3.h; the ONE delta is the
 * panel rail, which is the PMIC's PY_G2 here instead of a GPIO. Cross-checked
 * against M5Stack's docs, M5Unified/M5GFX and the K159-B schematic.
 *
 * Battery and buttons follow the PaperMono: the PMIC measures the cell
 * (src/m5pm1.c) and owns the power button, so there is no power latch and no
 * ADC divider on this board.
 *
 * Bench status 2026-10-26 (first unit): the -selftest paints 16 evenly spaced
 * grey bands on the PM1 rail alone, so no EPD_PIN_PWR 45 fallback is needed;
 * the red LED lights on cold boot and PWR_CFG reads 0x17 (DCDC and LDO on), so
 * pm1_rmw()'s guard accepts LED writes. Since then the production build was
 * flashed and verified on the same unit: Wi-Fi join, discover/status/frame over
 * HTTP against a real server, a composed 960x540 frame painted upright on the
 * glass, and timer deep-sleep wakes at the server's interval (`wakeup_cause=4`).
 * Both keys were then verified (2026-10-28): KEY1 on GPIO4 is the left key
 * (M5's "A") and KEY2 on GPIO3 the right ("B"), both waking the board from
 * deep sleep through ext1 and reporting themselves. Still unmeasured: battery
 * sag *under* paint - `battery_read_mv()` never runs inside `frame_paint()`,
 * so there is no instrument for it. See notes/paperdiy-bringup-2026-10-26.md.
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

/* Only the boost is still a GPIO. The panel rail (EPD_PWR) is the PMIC's
 * PY_G2, declared as EPD_PWR_M5PM1_PIN below. GPIO45 -- the PaperS3's rail --
 * is an expansion-port pin (EXT_G45) on this board and is left alone. If the
 * first selftest is blank, add `#define EPD_PIN_PWR 45` as an extra enable. */
#define EPD_PIN_BST_EN      46

/* The i80 driver requires a D/C pin the panel has no use for, so give it one
 * nothing else claims. Occupied: 3/4 keys, 6-18 + 46 panel, 19/20 USB,
 * 38-40 + 47/48 microSD, 41/42 I2C, 43/44 UART0 (freed by the USB-Serial-JTAG
 * console). GPIO21 is the expansion FPC's SPI chip-select, which this firmware
 * never drives. NOT 47 (SD CS) and NOT 48 (SD card-detect). */
#define EPD_PIN_DC_DUMMY    21

/* Trailing clocks per row: the source driver's shift register runs past the
 * visible glass, and without these the rightmost pixels never latch. M5GFX
 * runs this glass with 8 and a 16 MHz bus; these are the PaperS3's validated
 * values, the two numbers to try if a right-edge artefact shows. */
#define EPD_PAR_LINE_PADDING 16

/* Panel geometry, in the controller's own scan order: 960 across, 540 down.
 * 4bpp packed greyscale = 2 px/byte, so 259200 bytes a frame. Same as the
 * PaperS3 and the M5Paper. This board is used landscape: the server composes
 * 960x540, the same shape as the scan, and the frame packs straight through
 * with no rotation on the device (the catalog kind is landscape; the PaperS3
 * kind stays portrait). A frame painted upright on the glass confirmed it. */
#define EPD_WIDTH           960
#define EPD_HEIGHT          540
#define EPD_BUF_BYTES       ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 259200 */

/* Grey nibbles: 0x0 = black ... 0xF = white. */
#define EPD_COL_BLACK       0x0
#define EPD_COL_WHITE       0xF

#define EPD_PAR_PANEL_NAME  "Parallel EPD greyscale 4.7\" (960x540, 4bpp)"

/* Greyscale pass matrix: 16 rows (level 0 = black .. 15 = white) x 8 passes,
 * 2-bit drive code per entry (1 darken, 2 lighten, 0 neutral from white).
 *
 * Copied from the PaperS3's tuned table (boards/m5stack_papers3.h, derived in
 * notes/papers3-gray-matrix-tuning.md from measured band tones on this same
 * ED047TC1 glass). It is a starting point, not a guarantee: that tuning showed
 * per-unit waveform character, so judge the ramp on THIS unit first. */
#define EPD_PAR_GRAY_MATRIX { \
    /*  0 */ 1, 1, 1, 1, 1, 1, 1, 1, \
    /*  1 */ 2, 2, 1, 1, 2, 1, 1, 1, \
    /*  2 */ 2, 2, 1, 1, 1, 1, 2, 1, \
    /*  3 */ 2, 2, 2, 1, 1, 1, 2, 1, \
    /*  4 */ 2, 2, 1, 1, 2, 2, 1, 1, \
    /*  5 */ 1, 1, 1, 1, 1, 1, 2, 2, \
    /*  6 */ 2, 2, 1, 1, 2, 1, 1, 2, \
    /*  7 */ 2, 2, 2, 2, 1, 1, 2, 1, \
    /*  8 */ 2, 1, 2, 1, 1, 1, 2, 2, \
    /*  9 */ 2, 1, 2, 1, 1, 1, 2, 2, \
    /* 10 */ 2, 2, 2, 2, 2, 1, 2, 1, \
    /* 11 */ 1, 1, 1, 1, 2, 1, 2, 2, \
    /* 12 */ 2, 2, 1, 2, 1, 1, 2, 2, \
    /* 13 */ 2, 2, 1, 1, 2, 1, 2, 2, \
    /* 14 */ 2, 1, 1, 2, 2, 1, 2, 2, \
    /* 15 */ 2, 2, 2, 2, 2, 2, 2, 2 }

/* Candidate matrix for the selftest's side-by-side judgement (painted on the
 * bottom half of the sheet; normal display ignores it). The epdiy project's
 * hand-built ED047TC1 pulse-width ramp: level L darkens for 15-L passes from
 * white, then rests -- monotone by construction, so the failure to look for is
 * uneven spacing, never an inversion. Same table as the PaperS3 header. */
#define EPD_PAR_GRAY_MATRIX_B { \
    /*  0 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    /*  1 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, \
    /*  2 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, \
    /*  3 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, \
    /*  4 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, \
    /*  5 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, \
    /*  6 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, \
    /*  7 */ 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, \
    /*  8 */ 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, \
    /*  9 */ 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
    /* 10 */ 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
    /* 11 */ 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
    /* 12 */ 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
    /* 13 */ 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
    /* 14 */ 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
    /* 15 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }

/* VCOM is fixed in this board's supply hardware; there is no programmable
 * VCOM on the PMIC path. Documentation, not a setting. */
#define EPD_VCOM_MV         1600

/* ------------------------------------------------------------------ */
/* Panel power: rail on the M5PM1, boost on a GPIO                     */
/* ------------------------------------------------------------------ */
/* The PMIC's GPIO2 (docs: PY_G2) drives EPD_PWR. The driver routes its rail
 * set/clear through m5pm1_gpio_output() when this is defined. */
#define EPD_PWR_M5PM1_PIN   2

/* ------------------------------------------------------------------ */
/* System I2C bus: PMIC                                                 */
/* ------------------------------------------------------------------ */
/* SDA 41 / SCL 42 (M5Unified: the PaperDIY's internal bus), 100 kHz, as on
 * the PaperMono. M5PM1 at 0x6E. */
#define BOARD_M5PM1_I2C_PORT    0
#define BOARD_M5PM1_I2C_SDA     41
#define BOARD_M5PM1_I2C_SCL     42
#define BOARD_M5PM1_I2C_HZ      100000
#define BOARD_M5PM1_I2C_ADDR    0x6E

/* The cell is measured by the PMIC, not an ESP32 ADC pin. 140 mAh: autonomy
 * is days, so always-on mode is a USB-only affordance. */
#define BOARD_BATTERY_M5PM1     1

/* Red status LED on the PMIC's LED_EN, as on the PaperMono. m5pm1_led_set()
 * refuses PWR_CFG writes unless DCDC and LDO read as on; if this unit reads
 * otherwise the LED silently does nothing (check the "m5pm1: PWR_CFG" boot
 * line). */
#define BOARD_LED_M5PM1         1

/* ------------------------------------------------------------------ */
/* Buttons: two user keys, both RTC pads                                */
/* ------------------------------------------------------------------ */
/* KEY1 = GPIO4 -> refresh slot (reported as "left", the Xteink X4 / PaperMono
 * trick, so the server still binds it to rotate_prev); KEY2 = GPIO3 -> right.
 * Active low with external pull-ups to the always-on 3V3_L0 rail, so the
 * ESP32-S3's ext1 ANY_LOW wake covers BOTH keys in deep sleep. The power
 * button belongs to the PMIC (single press reset, double press off) and is not
 * a GPIO. Verified on hardware: GPIO4 is the key on the left (M5 "A"), GPIO3
 * the one on the right ("B"). */
#define BOARD_BTN_REFRESH_PIN        4
#define BOARD_BTN_REFRESH_WIRE_NAME  "left"
#define BOARD_BTN_RIGHT_PIN          3

/* ------------------------------------------------------------------ */
/* Not present: touch, buzzer, frontlight                               */
/* ------------------------------------------------------------------ */

/* Board model -> default device id "M5PaperDIY_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "M5PaperDIY"

/* BLE hardware code, unique across boards/ (see docs/ble-setup-protocol.md). */
#define TESSERAE_BLE_HARDWARE_CODE  13

/* Tesserae hardware-catalog kind. Selects the server-side renderer and .bin
 * packer: esp32_gray_bin, the same 4bpp linear-greyscale wire format the
 * PaperS3 and M5Paper use, at 960x540 (259200 B). A kind is also the OTA
 * lineage, so it is fixed from first release. See
 * hardware/m5stack/paperdiy.json in the Tesserae repo. */
#define TESSERAE_DEVICE_KIND   "m5stack_paperdiy"

/* Cloud-relay self-report. Hardware facts, same family as the PaperS3. */
#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "gray_16"

/* MCU tier: ESP32-S3 + 8 MB octal PSRAM. */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Selected panel driver: Family F, raw parallel glass in 16-level grey. */
#define PANEL_DRIVER_PARALLEL_EPD_GRAY 1
