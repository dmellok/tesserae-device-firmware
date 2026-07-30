/*
 * touch3_run.h: wake-loop orchestration for touch v3 (device-owned touch).
 *
 * Ties touch3.h (pure spec/geometry/draw engine) to net_rest (spec fetch,
 * /interact reports), the panel (through overlay_run's framebuffer + hygiene
 * accessors), and the SD card (spec + atlas cache keyed by layout_digest /
 * atlas digest, so a touch wake can hit-test and feed back before any network
 * round trip).
 *
 * Flow per frame (firmware-spec §4):
 *
 *   FETCH   /frame -> image
 *           GET /frame/spec?layout=<held> -> parse -> atlases
 *   RENDER  compose primitives INTO the decoded frame, then one GC16 paint
 *   IDLE    always_on: hold SSE; battery: sleep, wake on GT911
 *   TOUCH   hit-test -> local DU feedback FIRST -> /interact -> (reconcile)
 *
 * Version fallback: the SPEC PULL is the v3 signal. ?layout= is advisory -- the
 * server answers for the device's current frame regardless -- so the returned
 * layout_digest is what detects a layout change, and a 404/204 is what means "no
 * v3 for this frame". Then touch3_try_touch() returns false and the caller runs
 * the v2 (proto2) then v1 coordinate dispatch unchanged. The legacy paths stay
 * live until the server side carries load; the contract's Phase 3 removes them
 * (notes/design-handoffs/touch-v3/README.md).
 *
 * Server gaps as of the 2026-07-27 sync, both handled by degrading, not by
 * waiting: /interact replies carry no confirmed state (so an optimistic draw
 * STAYS optimistic until the SSE values channel lands), and specs carry no
 * atlases (so primitives render chrome-only, text arrives later).
 *
 * Compiled to no-op stubs unless BOARD_OVERLAY_PARTIAL && BOARD_HAS_TOUCH.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

#if defined(BOARD_OVERLAY_PARTIAL) && defined(BOARD_HAS_TOUCH)
#define BOARD_TOUCH3 1

/* Boot hook (after rest_config_load + sdcard_mount): restore the SD-cached
 * spec + atlases for the layout last painted, so a touch wake can hit-test
 * and draw feedback with the radio still down. */
void touch3_boot(void);

/* A /frame 200 arrived, before the paint. layout_digest comes from the
 * response ("" = no v3 for this frame: releases any held spec). Fetches and
 * parses the spec on a cache miss, plus any uncached atlases. Radio must be
 * up. */
void touch3_frame_downloaded(const char *frame_digest,
                             const char *layout_digest);

/* Re-pull the spec even though no new frame arrived. The normal pull rides a
 * frame download, so a page whose IMAGE is unchanged (304 on every poll) would
 * never be re-checked -- enabling controls server-side on such a page would go
 * unnoticed forever. Call this from the 304 path when no controls are held.
 * Radio must be up. */
void touch3_poll_spec(void);

/* True once (clears on read) when a spec pull gained controls where there were
 * none. The frame on glass was composed WITHOUT them, so the caller must force
 * a full frame refetch + repaint for them to appear. Mirrors
 * overlay_take_refetch(). */
bool touch3_take_repaint(void);

/* Compose the primitives INTO a decoded full frame, in place, before it is
 * pushed to the panel. The server left those rects blank; this fills them, so
 * the frame's single GC16 shows image and controls together (firmware-spec §4
 * RENDER) instead of N partial refreshes after the fact. No-op when no spec is
 * held. fb must be a panel-native full frame. */
void touch3_compose(uint8_t *fb);

/* The composed frame is on glass. Records the layout on display (persisted for
 * the next boot's cache restore) and resets the per-rect hygiene counters. */
void touch3_after_paint(const char *frame_digest);

/* Touch entry point, called BEFORE the v2/v1 dispatch. True = the stroke was
 * v3-owned: hit-tested, feedback applied, report sent or queued (a miss inside
 * a v3 layout is silently dropped -- the device owns the whole surface).
 * False = no v3 spec for the layout on glass, so the caller must run its v2
 * then v1 dispatch unchanged. *want_frame_poll is set when the action needs a
 * new frame to settle (tier 2, nav/refresh/fetch). */
bool touch3_try_touch(int x0, int y0, int x1, int y1, uint32_t ms,
                      bool *want_frame_poll);

/* Live-drag hook for touch_capture_stroke_cb(): called per GT911 sample while
 * a finger is down. On a slider it moves the thumb and repaints value_text
 * with a DU refresh (rate-limited -- a DU costs ~250-450 ms, far longer than
 * the 8 ms sample interval), so the control tracks the finger instead of
 * jumping on release. Cheap no-op for every other primitive. */
void touch3_stroke_sample(int fx, int fy, void *ctx);

/* Flush queued /interact reports (call from linger idle / after reconnect). */
void touch3_flush_reports(void);

/* True while a v3 spec with at least one primitive is held for the layout on
 * glass (main uses this to floor the linger window, and to skip the v2 path). */
bool touch3_active(void);

/* A values envelope arrived -- {"seq":<ms>,"values":{"<value_key>":"<str>"}} --
 * from the SSE `values` event or a polled /status response. Keyed by VALUE_KEY,
 * so the key -> primitive mapping is done here; every primitive bound to a
 * changed key is updated and partial-redrawn, which is how a switch follows
 * Home Assistant when it changes externally. This is the ONLY reconcile path:
 * /interact replies carry no confirmed state. Newest seq wins. */
void touch3_ingest_values(const char *json, size_t len);

/* The layout digest currently held, "" when none. For the SSE client, which
 * prefers the v3 /frame/stream endpoint while a v3 spec is live. */
const char *touch3_layout_digest(void);

#else /* !touch3 boards */

#define BOARD_TOUCH3 0

static inline void touch3_boot(void) { }
static inline void touch3_frame_downloaded(const char *f, const char *l)
{ (void)f; (void)l; }
static inline void touch3_poll_spec(void) { }
static inline bool touch3_take_repaint(void) { return false; }
static inline void touch3_compose(uint8_t *fb) { (void)fb; }
static inline void touch3_after_paint(const char *f) { (void)f; }
static inline bool touch3_try_touch(int x0, int y0, int x1, int y1,
                                    uint32_t ms, bool *want_frame_poll)
{ (void)x0; (void)y0; (void)x1; (void)y1; (void)ms;
  if (want_frame_poll) *want_frame_poll = false; return false; }
static inline void touch3_stroke_sample(int fx, int fy, void *ctx)
{ (void)fx; (void)fy; (void)ctx; }
static inline void touch3_flush_reports(void) { }
static inline bool touch3_active(void) { return false; }
static inline void touch3_ingest_values(const char *j, size_t l)
{ (void)j; (void)l; }
static inline const char *touch3_layout_digest(void) { return ""; }

#endif
