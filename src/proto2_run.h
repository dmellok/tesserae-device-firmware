/*
 * proto2_run.h: wake-loop orchestration for protocol v2 (device-owned touch).
 *
 * Ties proto2.h (pure manifest/gesture/hit-test engine) to the panel
 * (through overlay_run's framebuffer + hygiene accessors), net_rest
 * (manifest fetch, /tap reports), and the SD card (manifest cache keyed by
 * frame digest). Implements the tier engine:
 *
 *   tier 0  local feedback, report async, no correction expected
 *   tier 1  optimistic feedback + ledger entry, server corrects via
 *           patches/frames (server-wins: the device never self-reverts)
 *   tier 2  invert echo, report, the caller polls /frame for the result
 *
 * v1 fallback (proto2 §10): when the server carries no manifest signal,
 * proto2_try_touch() returns false and the caller runs today's
 * coordinate-stroke dispatch unchanged. Probes are cached per boot (a
 * deep-sleep wake is a connection epoch).
 *
 * Compiled to no-op stubs unless BOARD_OVERLAY_PARTIAL && BOARD_HAS_TOUCH.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

#if defined(BOARD_OVERLAY_PARTIAL) && defined(BOARD_HAS_TOUCH)
#define BOARD_PROTO2 1

/* Boot hook (after rest_config_load + sdcard_mount): restore the SD-cached
 * manifest for the frame on glass, so a touch wake can hit-test before any
 * network round trip. */
void proto2_boot(void);

/* A /frame 200 arrived. digest = the new frame's digest; manifest_digest /
 * manifest_url come from the response's manifest block ("" when absent --
 * that absence marks the server v1 for this epoch). Fetches or re-anchors
 * the manifest (same manifest_digest as held = keep, no refetch). */
void proto2_frame_downloaded(const char *digest, const char *manifest_digest,
                             const char *manifest_url);

/* Touch entry point, called INSTEAD of the v1 stroke dispatch when it
 * returns true: the stroke was v2-owned (hit-tested; feedback applied and
 * report sent/queued on a hit; silently dropped on a miss, per §5).
 * False = no manifest for the frame on glass / v1 epoch: run the v1
 * coordinate dispatch unchanged. *want_frame_poll is set true when the
 * action expects a new frame (tier 2, nav/refresh/fetch_latest) and the
 * caller should follow up with its frame poll. */
bool proto2_try_touch(int x0, int y0, int x1, int y1, uint32_t ms,
                      bool *want_frame_poll);

/* Flush queued /tap reports (call from linger idle / after reconnect). */
void proto2_flush_reports(void);

/* Server-wins hooks: a painted full frame clears the whole optimistic
 * ledger; overlay_run reports applied patch rects itself. */
void proto2_frame_painted(const char *digest);

/* True while a v2 manifest with at least one region is held for the frame
 * on glass (main uses this to floor the linger window). */
bool proto2_active(void);

#else /* !proto2 boards */

static inline void proto2_boot(void) { }
static inline void proto2_frame_downloaded(const char *d, const char *md,
                                           const char *mu)
{ (void)d; (void)md; (void)mu; }
static inline bool proto2_try_touch(int x0, int y0, int x1, int y1,
                                    uint32_t ms, bool *want_frame_poll)
{ (void)x0; (void)y0; (void)x1; (void)y1; (void)ms;
  if (want_frame_poll) *want_frame_poll = false; return false; }
static inline void proto2_flush_reports(void) { }
static inline void proto2_frame_painted(const char *d) { (void)d; }
static inline bool proto2_active(void) { return false; }

#endif
