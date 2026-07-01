/*
 * Family D: IT8951 controller over SPI, grayscale (4bpp / GC16).
 *
 * Concrete driver for the Seeed reTerminal E1003's 10.3" ED103TC2 grayscale
 * panel. The ESP32 talks to an IT8951 timing controller over a bidirectional
 * SPI link (16-bit-word protocol with command/data/read preambles + HRDY
 * handshaking); the IT8951 buffers the image in its own DRAM and drives the
 * glass. Ported from bitbank2 FastEPD's IT8951 path. See the .c.
 */
#pragma once

#include "panel/epd_panel.h"

extern const epd_driver_t it8951_gray_driver;
