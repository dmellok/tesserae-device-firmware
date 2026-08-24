/*
 * Family F: raw parallel e-paper glass, 16-level grayscale.
 *
 * The first driver here that does NOT talk SPI. There is no controller chip
 * between the ESP32 and the glass: the MCU drives the panel's source driver
 * over an 8-bit parallel bus (the ESP32-S3's LCD/i80 peripheral, one byte =
 * four pixels of 2-bit drive code) and clocks the gate driver by hand on
 * SPV/CKV. Waveforms are not in a controller's flash, they ARE this file --
 * the 16 grey levels come out of a per-level pass matrix that says, for each
 * of N passes, whether to push that pixel toward black or toward white.
 *
 * Target: M5Stack PaperS3 (4.7" ED047TC1, 960x540). Also the shape every
 * other epdiy-class board takes (Inkplate, LilyGo T5, epdiy V7), so a second
 * board of that family is a pin map plus a matrix, not a new driver.
 *
 * Sequences ported from bitbank2 FastEPD (BB_PANEL_M5PAPERS3: PaperS3EinkPower,
 * PaperS3IOInit, PaperS3RowControl, bbepClear, bbepFullUpdate's 4bpp path).
 * Ported rather than vendored, for the reason the project always ports: a panel
 * library release that retunes one waveform otherwise moves every panel we
 * support at once.
 *
 * Frame format is the same 4bpp packed grayscale the IT8951 driver takes --
 * W*H/2 bytes, high nibble = left pixel, 0x0 black .. 0xF white -- so the
 * server's existing esp32_gray_bin renderer and gray_16 gamut cover it with no
 * new packer.
 *
 * UNVERIFIED ON HARDWARE.
 */
#pragma once

#include "panel/epd_panel.h"

extern const epd_driver_t parallel_epd_gray_driver;
