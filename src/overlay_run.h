/*
 * overlay_run.h: wake-loop orchestration for the local overlay render mode.
 *
 * Ties overlay.h (pure engine) to the panel's partial refresh, net_rest
 * (spec/values/atlas fetch), and the SD card (spec + atlas + rect-patch
 * cache keyed by frame digest, so a tap that wakes the device can echo
 * before any network round trip). Compiled to no-op stubs on boards without
 * BOARD_OVERLAY_PARTIAL; at runtime everything degrades to dormant on any
 * failure (404, malformed spec, missing atlas) -- zero behaviour change
 * against a server that predates the feature.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"   /* boards/board.h: BOARD_OVERLAY_PARTIAL */

#if defined(BOARD_OVERLAY_PARTIAL)

/* Boot hook (after rest_config_load + sdcard mount): restore the SD-cached
 * spec for the currently displayed frame. Cold wakes carry slot patches
 * only -- values arriving on /status can redraw offline; target echo needs
 * a full in-RAM frame copy and is skipped on such wakes (see overlay_run.c). */
void overlay_boot(void);

/* Tap echo: if (x, y) hits a declared target, apply its echo (invert) and
 * partial-refresh that rect IMMEDIATELY. Returns true when an echo was
 * painted. NEVER swallows the event -- the caller dispatches the stroke to
 * the server exactly as today regardless. */
bool overlay_try_echo(int x, int y);

/* Radio-up hook, after a new frame body was downloaded (before painting):
 * fetch the overlay spec + any uncached atlases for `digest`. 404 = feature
 * off for this frame. Drops any previous frame's overlay state. */
void overlay_frame_downloaded(const char *digest);

/* After the full frame was painted: keep the base framebuffer copy, extract
 * + SD-cache the target/slot rect patches, reset the hygiene counter. */
void overlay_after_paint(const uint8_t *frame, const char *digest);

/* overlay_values object arrived on a /status response (raw JSON). Same
 * semantics as the polled values document; newest seq wins. */
void overlay_ingest_values(const char *json, size_t len);

/* Call every 1-2 s from the touch-linger window (radio up, awake): poll
 * GET /frame/data, apply, redraw + partial-refresh changed slots. NEVER
 * called outside the awake window -- the device must not wake for values. */
void overlay_linger_poll(void);

#else /* !BOARD_OVERLAY_PARTIAL */

static inline void overlay_boot(void) { }
static inline bool overlay_try_echo(int x, int y) { (void)x; (void)y; return false; }
static inline void overlay_frame_downloaded(const char *d) { (void)d; }
static inline void overlay_after_paint(const uint8_t *f, const char *d) { (void)f; (void)d; }
static inline void overlay_ingest_values(const char *j, size_t l) { (void)j; (void)l; }
static inline void overlay_linger_poll(void) { }

#endif /* BOARD_OVERLAY_PARTIAL */
