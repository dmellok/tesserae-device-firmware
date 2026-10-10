/*
 * Family G: raw parallel e-paper glass on an epdiy board, 16-level grayscale,
 * driven through the epdiy library (vroland/epdiy, LGPL-3.0).
 *
 * The PaperS3's driver (parallel_epd_gray, Family F) ports FastEPD's sequences
 * so a library release cannot retune a shipped panel. epdiy boards are the
 * exception: the v7 board puts the panel rails behind a TPS65185 PMIC and the
 * control lines behind a PCA9555 expander, and epdiy's own board definition is
 * the reference for both, along with waveforms for the glass it supports. So
 * this driver links epdiy, pinned to one exact version in
 * components_opt/epdiy_dep/idf_component.yml, and only translates between the
 * Tesserae frame and epdiy's framebuffer. A version bump is a deliberate edit.
 *
 * The board header picks the epdiy board and display (EPD_EPDIY_BOARD,
 * EPD_EPDIY_DISPLAY), the panel's VCOM (EPD_EPDIY_VCOM_MV, printed on the
 * panel's flex cable), and optionally a waveform (EPD_EPDIY_WAVEFORM; default
 * is the display's own). A new epdiy panel is a board header, not a driver.
 *
 * Frame format is the 4bpp packed grayscale every grey board here takes --
 * W*H/2 bytes, high nibble = left pixel, 0x0 black .. 0xF white -- so the
 * server's esp32_gray_bin renderer and gray_16 gamut cover it. epdiy keeps the
 * left pixel in the LOW nibble, so the copy swaps each byte's nibbles.
 *
 * UNVERIFIED ON HARDWARE.
 */
#pragma once

#include "panel/epd_panel.h"

extern const epd_driver_t epdiy_gray_driver;
