/*
 * Board variant: XIAO ePaper driver board + 7.5" BLACK/WHITE/RED panel
 * (DKE DEPG0750RW family / GoodDisplay GDEY075Z08 class -- the tri-color
 * sibling of the TRMNL kit's mono glass, same 800x480 UC8179-class outline;
 * commonly sold on AliExpress as a "7.5 inch for Waveshare/TRMNL" panel).
 *
 * Same hardware as seeed_xiao_epaper_75.h -- this variant re-keys the wire
 * format and driver mode only:
 *   - Frame: 2bpp packed, 4 px/byte, MSB-first (bits 7-6 = leftmost pixel),
 *     values 0b00 = black, 0b01 = white, 0b10 = red (0b11 reserved, rendered
 *     white), 96000 bytes.
 *   - Driver: mono_spi in EPD_BWR mode -- KWR OTP waveform, dual-plane
 *     (DTM1 = B/W, DTM2 = red). Tri-color full refresh is SLOW (~15-25 s of
 *     visible flashing) -- that is normal for this glass.
 *   - Kind: xiao_epaper_75_bwr (server renders 96000-byte 2bpp BWR).
 *     MODEL stays XIAO_ePaper_75 so the device id is stable across variant
 *     reflashes; the server auto-heals the kind on re-register/discover.
 */
#pragma once

#include "seeed_xiao_epaper_75.h"

/* 2bpp packed frame replaces the 1bpp one. */
#undef  EPD_BUF_BYTES
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 4)   /* 2bpp packed = 96000 */

/* BWR palette. White stays 0x1 (as mono); red is new. */
#define EPD_COL_RED    0x2

/* Server-side renderer key. Model deliberately NOT overridden (see above). */
#undef  TESSERAE_DEVICE_KIND
#define TESSERAE_DEVICE_KIND  "xiao_epaper_75_bwr"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair).
 * Values come from this board's entry in the Tesserae hardware
 * catalog (hardware/<vendor>/xiao_epaper_75_bwr.json): "protocol" is the
 * device KIND that selects the renderer/.bin packer, and
 * "panel.gamut" is the palette the server quantizes to. Both are
 * hardware facts, so the panel reports them at pairing and the
 * operator no longer has to pre-enter them.
 *
 * NOTE the kind is NOT the catalog id above -- that is a hardware id, not a
 * device kind. Getting this wrong silently mis-packs every frame. */
/* The base header above already defined these for the mono panel; this
 * variant has a different gamut, so undef first -- same pattern the
 * file already uses for EPD_BUF_BYTES and TESSERAE_DEVICE_KIND. */
#undef  TESSERAE_RELAY_MODEL
#undef  TESSERAE_RELAY_GAMUT
#define TESSERAE_RELAY_MODEL   "esp32_bw_client"
#define TESSERAE_RELAY_GAMUT   "bwr_3"

/* Flip mono_spi into the dual-plane KWR mode. */
#define EPD_BWR 1
