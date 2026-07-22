/*
 * Board: Waveshare ESP32-S3 PhotoPainter (7.3")
 *   - MCU:   ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM)
 *   - Panel: 7.3" 800x480 6-colour Spectra E6 (ED2208-GCA), single controller
 *   - Power: AXP2101 PMIC over I2C (panel rails + battery fuel gauge)
 *
 * Family B. The ED2208-GCA init sequence is byte-identical to the E1002's
 * spectra6_spi_single driver, so this board REUSES that driver, adding two
 * board-gated behaviours it flips on:
 *   - EPD_ROTATE_180: the panel is mounted 180 degrees in the PhotoPainter
 *     case, so the driver reverses the byte stream + swaps nibbles on the
 *     full-frame send.
 *   - BOARD_HAS_PMIC: panel power is an AXP2101 LDO rail (not a GPIO gate);
 *     the driver brings the rails up via pmic_*() around init.
 *
 * Frame is the same 192000-byte 4bpp packed Spectra layout as the E1002, so
 * the server reuses that renderer under TESSERAE_DEVICE_KIND below.
 *
 * Pinout from waveshareteam/ESP32-S3-PhotoPainter (bsp_config.h / I2C example)
 * and the tesserae-photopainter-7.3-bin-client port.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (single chip-select, SPI3)                            */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   10
#define EPD_PIN_MOSI   11
#define EPD_PIN_CS     9
#define EPD_PIN_DC     8
#define EPD_PIN_RST    12
#define EPD_PIN_BUSY   13   /* active low: 0 = busy */

#define EPD_SPI_HOST   SPI3_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)   /* 10 MHz (reference value) */

/* Panel geometry. Landscape-native 800x480, 4bpp packed = 192000 bytes. */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)

/* The panel is mounted upside-down in the case: rotate the frame 180 in the
 * driver (reverse byte order + swap each byte's nibbles). */
#define EPD_ROTATE_180 1

/* 6-colour Spectra palette indices (nibble values). Matches the E1002. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3
#define EPD_COL_BLUE    0x5
#define EPD_COL_GREEN   0x6

/* ------------------------------------------------------------------ */
/* AXP2101 PMIC over I2C: panel rails + battery (no GPIO gate/ADC)      */
/* ------------------------------------------------------------------ */
#define BOARD_HAS_PMIC        1
#define PMIC_I2C_PORT         0
#define PMIC_I2C_SCL          48
#define PMIC_I2C_SDA          47
#define PMIC_I2C_HZ           100000   /* 400 kHz errors on this board's trace */
#define PMIC_AXP2101_ADDR     0x34
#define PMIC_PIN_IRQ          21

/* Battery telemetry comes from the AXP2101 fuel gauge, not an ADC divider.
 * battery.c routes battery_read_mv() through pmic_battery_mv() for this flag. */
#define BOARD_BATTERY_PMIC    1

/* ------------------------------------------------------------------ */
/* Onboard SHTC3 temperature/humidity sensor (U3, Sensirion).          */
/* From ESP32-S3-PhotoPainter-Schematic-v2.0: U3 sits on the SAME I2C  */
/* bus as the AXP2101 -- port 0, SDA GPIO47 / SCL GPIO48 (the ESP_I2C  */
/* net, external 4.7k pull-ups R30/R33). The SHTC3 has a single fixed  */
/* address (0x70) and cannot share the AXP's 0x34, so both coexist on  */
/* one bus. The shtc3 driver REUSES the bus the PMIC already created    */
/* (see shtc3.c) rather than making a second one on port 0.            */
#define BOARD_HAS_SHTC3        1
#define BOARD_SHTC3_I2C_PORT   0
#define BOARD_SHTC3_I2C_SDA    47
#define BOARD_SHTC3_I2C_SCL    48
#define BOARD_SHTC3_I2C_HZ     100000   /* match the PMIC: 400 kHz errors on this trace */
#define BOARD_SHTC3_I2C_ADDR   0x70

/* Board model -> default device id "PhotoPainter_73_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "PhotoPainter_73"

/* Tesserae hardware-catalog kind. Same 800x480 4bpp Spectra-6 frame (192000
 * bytes) as the E1002, so the server maps this to the existing renderer. */
#define TESSERAE_DEVICE_KIND   "waveshare_photopainter_73"

/* MCU tier: ESP32-S3 + octal PSRAM. */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Selected panel driver: Family B, single-controller Spectra-6 (shared with
 * the E1002; EPD_ROTATE_180 + BOARD_HAS_PMIC above tailor it to this board). */
#define PANEL_DRIVER_SPECTRA6_SPI_SINGLE 1
