/*
 * Board: Seeed reTerminal E1004
 *   - MCU:   XIAO ESP32-S3 class (ESP32-S3 + PSRAM)
 *   - Panel: 13.3" T133A01, dual-chip Spectra-6, 1200x1600
 *
 * IMPORTANT: despite being another "13.3" dual-controller Spectra-6 1200x1600"
 * panel, the T133A01 is NOT the same silicon as the Waveshare 13.3E6
 * (GDEP133C02). Its init sequence, refresh command (DRF=0x01), and per-frame
 * CCSET differ. It therefore uses its own driver (spectra6_t133a01_dual),
 * ported verbatim from the limengdu/bb_epaper fork (bbepT133A01InitIO /
 * bbepWriteImage4bppDual / bbepRefresh / bbepSleep) pinned at commit
 * 95fd94afe39cd7db32bef7c70eea06d654264ff6, which underlies the reference bb_epaper fork.
 *
 * Pin map from the reference (src/DEV_Config.h, BOARD_SEEED_RETERMINAL_E1004).
 * Panel pins are named against the shared driver's roles:
 *   CS_M = primary controller (CS,  drives left  half, cols   0..599)
 *   CS_S = second  controller (CS1, drives right half, cols 600..1199)
 *   PWR  = EN, the active-high panel power enable
 *
 * UNVERIFIED ON HARDWARE. Confirm pins, PSRAM mode (oct vs quad), flash size
 * (esptool flash-id), and the init/refresh sequence on a real E1004 before
 * trusting a flash. See tesserae-device-firmware-HANDOVER.md.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (XIAO ESP32-S3 GPIO numbers)                          */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   7
#define EPD_PIN_MOSI   9
#define EPD_PIN_CS_M   10   /* primary controller (CS)  -> left  half  */
#define EPD_PIN_CS_S   2    /* second controller  (CS1) -> right half  */
#define EPD_PIN_DC     11
#define EPD_PIN_RST    38
#define EPD_PIN_BUSY   13
#define EPD_PIN_PWR    12   /* EN: active-high panel power enable       */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. Native orientation is portrait. Identical dimensions and
 * 4bpp packed layout to the base 13.3E6, so image_decoder/splash blobs and
 * the server-side 4bpp Spectra-6 renderer are reused unchanged. */
#define EPD_WIDTH      1200
#define EPD_HEIGHT     1600
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 4bpp packed = 960000 */

/* 6-color Spectra palette indices (nibble values). Same LUT as 13.3E6. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3
#define EPD_COL_BLUE    0x5
#define EPD_COL_GREEN   0x6

/* ------------------------------------------------------------------ */
/* MCU tier: ESP32-S3 + PSRAM (assumed octal, verify on hardware).     */
/* ------------------------------------------------------------------ */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* ------------------------------------------------------------------ */
/* Selected panel driver (see src/panel/registry.c).                   */
/* Family A variant: Spectra-6 SPI dual-controller, T133A01 silicon.   */
/* ------------------------------------------------------------------ */
#define PANEL_DRIVER_SPECTRA6_T133A01_DUAL 1
