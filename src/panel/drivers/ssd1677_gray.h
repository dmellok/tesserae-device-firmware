/*
 * Family E: SSD1677 4-level grayscale over SPI, 800x480.
 *
 * The panel in the Seeed reTerminal Sticky, and the same controller Seeed's own
 * board setups use for the XIAO ePaper 3.97" and the (unannounced at time of
 * writing) reTerminal E1005. One driver covers all three: the Sticky's
 * documented pin map is identical to Seeed_GFX's E1005 setup, pin for pin,
 * which is what identifies the controller -- the Sticky's own docs never name
 * it.
 *
 * NOT a UC8179 despite the identical geometry and wire format. This is an
 * SSD16xx-class part: a different command set (0x24/0x26 RAM planes, 0x22+0x20
 * to trigger, 0x44/0x45 windowing), a different init, and -- the easy one to
 * get wrong -- INVERTED BUSY. On the UC8179 panels BUSY low means busy; here
 * BUSY high means busy.
 *
 * The frame is the same 2bpp packed format as the E1001 gray build:
 * EPD_BUF_BYTES = W*H/4 = 96000 bytes, 4 px/byte, MSB-first, 0b00 = black ..
 * 0b11 = white. That is deliberate and worth keeping: the server renderer, the
 * .bin packer and the gray_4 gamut already exist for that shape, so this panel
 * needed no server work at all.
 *
 * Ported from Seeed_GFX TFT_Drivers/SSD1677_Defines.h. UNVERIFIED ON HARDWARE.
 */
#pragma once

#include "panel/epd_panel.h"

extern const epd_driver_t ssd1677_gray_driver;
