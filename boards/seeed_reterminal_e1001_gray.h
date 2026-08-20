/*
 * Board variant: Seeed reTerminal E1001, 4-level GRAYSCALE build (Family C).
 *
 * Same hardware as seeed_reterminal_e1001.h -- this variant re-keys the wire
 * format and driver mode only:
 *   - Frame: 2bpp packed, 4 px/byte, MSB-first (bits 7-6 = leftmost pixel),
 *     values linear 0b00 = black .. 0b11 = white, 96000 bytes. No mirror in
 *     the payload (panel-side handling lives in the driver, as on the E1003).
 *   - Driver: mono_spi in EPD_GRAY4 mode -- dual-plane (DTM1+DTM2) refresh,
 *     driven either by the panel's built-in OTP 4-gray waveform or by register
 *     LUTs, whichever this glass actually has. mono_spi asks the controller at
 *     port init (gray_probe_waveform) rather than being told at build time, so
 *     this one target covers both batches. The 1bpp OTP-waveform env stays
 *     untouched; pure B/W genuinely looks better there, so gray is a separate
 *     opt-in build.
 *   - Kind: seeed_reterminal_e1001_gray (server renders 96000-byte 2bpp).
 *     The MODEL stays reTerminal_E1001 so the default device id is stable
 *     across mono<->gray reflashes; the server auto-heals the registration's
 *     kind on the next register/discover (MAC-matched).
 */
#pragma once

#include "seeed_reterminal_e1001.h"

/* 2bpp packed frame replaces the 1bpp one. */
#undef  EPD_BUF_BYTES
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 4)   /* 2bpp packed = 96000 */

/* 4-gray palette (linear). White moves from bit-value 1 to 0b11. */
#undef  EPD_COL_WHITE
#define EPD_COL_BLACK      0x0
#define EPD_COL_DARKGRAY   0x1
#define EPD_COL_LIGHTGRAY  0x2
#define EPD_COL_WHITE      0x3

/* Server-side renderer key. Model deliberately NOT overridden (see above). */
#undef  TESSERAE_DEVICE_KIND
#define TESSERAE_DEVICE_KIND  "seeed_reterminal_e1001_gray"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair).
 * Values come from this board's entry in the Tesserae hardware
 * catalog (hardware/<vendor>/seeed_reterminal_e1001_gray.json): "protocol" is the
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
#define TESSERAE_RELAY_GAMUT   "gray_4"

/* Flip mono_spi into dual-plane 4-level grayscale. The waveform (built-in OTP
 * vs uploaded register LUTs) is probed at runtime; -DGRAY4_WAVEFORM=0 or 1
 * pins it if a panel ever needs overriding. */
#define EPD_GRAY4 1
