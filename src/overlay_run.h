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
#include "panel/epd_panel.h"   /* epd_refresh_t */

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

/* A patch document arrived (schema 2: post-action frame patches) -- either
 * the raw "overlay_patches" object off a /status response or a full
 * /frame/data body carrying "patches". Digest-anchored to the frame on
 * glass; fetches the rect blob and partial-refreshes the changed rects.
 * The frame digest never changes: the patches ARE the repaint. */
void overlay_ingest_patches(const char *json, size_t len);

/* True once (clears on read) when a patch could not be honoured and the
 * contract fallback applies: run a normal /frame poll (fetch_and_paint).
 * Poll from the linger loop right after overlay_linger_poll(). */
bool overlay_take_refetch(void);

/* Call every ~1 s from the touch-linger window (radio up, awake): poll
 * GET /frame/data, apply values + patches, partial-refresh what changed.
 * NEVER called outside the awake window -- the device must not wake for
 * values or patches. */
void overlay_linger_poll(void);

/* ---- framebuffer access for proto2_run (v2 tier engine) ----
 * overlay_run owns the in-RAM frame copies and the partial-refresh hygiene
 * counter; proto2 composites its feedback (inverts, tiles, text) through
 * these instead of duplicating 2 x 1.3 MB of PSRAM buffers. */

/* Working framebuffer (what partials stream from), or NULL when this wake
 * has no full copy. full=true only when it holds the complete frame (not a
 * sparse SD reconstruction). */
uint8_t *overlay_work_fb(bool *full);
/* Pristine base copy (server frame without local composites); NULL when
 * absent or sparse. */
uint8_t *overlay_base_fb(void);
/* Partial-refresh a rect from the work buffer through the shared hygiene
 * counter (DU when fast). Initialises the panel port on demand. */
void overlay_partial_refresh(int x, int y, int w, int h, bool fast);

/* As above with an explicit waveform (touch v3 uses EPD_RF_A2 for feedback,
 * where latency is what the user feels). Shares the SAME hygiene counter, so
 * A2's heavier ghosting cannot buy itself a longer leash. */
void overlay_partial_refresh_mode(int x, int y, int w, int h,
                                  epd_refresh_t mode);

#else /* !BOARD_OVERLAY_PARTIAL */

static inline void overlay_boot(void) { }
static inline bool overlay_try_echo(int x, int y) { (void)x; (void)y; return false; }
static inline void overlay_frame_downloaded(const char *d) { (void)d; }
static inline void overlay_after_paint(const uint8_t *f, const char *d) { (void)f; (void)d; }
static inline void overlay_ingest_values(const char *j, size_t l) { (void)j; (void)l; }
static inline void overlay_ingest_patches(const char *j, size_t l) { (void)j; (void)l; }
static inline bool overlay_take_refetch(void) { return false; }
static inline void overlay_linger_poll(void) { }
static inline uint8_t *overlay_work_fb(bool *full) { if (full) *full = false; return (uint8_t *)0; }
static inline uint8_t *overlay_base_fb(void) { return (uint8_t *)0; }
static inline void overlay_partial_refresh(int x, int y, int w, int h, bool fast)
{ (void)x; (void)y; (void)w; (void)h; (void)fast; }
static inline void overlay_partial_refresh_mode(int x, int y, int w, int h,
                                                epd_refresh_t mode)
{ (void)x; (void)y; (void)w; (void)h; (void)mode; }

#endif /* BOARD_OVERLAY_PARTIAL */
