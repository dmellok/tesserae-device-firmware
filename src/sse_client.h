/*
 * sse_client.h: Server-Sent Events client for the proto2 push transport.
 *
 * GET /api/v1/device/<id>/stream (Bearer, Accept: text/event-stream).
 * Only meaningful under an always-on power policy (kiosk mode): a
 * deep-sleeping device cannot hold the connection, and the contract makes
 * SSE an optimisation, never a correctness requirement -- polling carries
 * the same envelopes.
 *
 * Usage (from the kiosk loop): sse_pump() every iteration. It opens /
 * re-opens the stream as needed (exponential backoff 1..60 s, +-25 %
 * jitter), reads whatever is available within a short budget, and
 * dispatches complete events:
 *   values  -> overlay_ingest_values + proto2_ingest_values
 *   patches -> overlay_ingest_patches
 *   sync    -> proto2_note_sync (resync executed by the caller)
 * Unknown event names are ignored. A 60 s silence (no event, no ":ka"
 * keepalive) forces a reconnect. sse_stop() closes cleanly before sleep.
 */
#pragma once

#include <stdbool.h>

#include "app_config.h"

#if defined(BOARD_OVERLAY_PARTIAL) && defined(BOARD_HAS_TOUCH)

/* Pump the stream: connect/read/dispatch within ~read_budget_ms. Returns
 * true while the stream is healthy (connected or backing off normally). */
bool sse_pump(int read_budget_ms);

/* True when the stream is currently connected (callers may relax their
 * fallback polling cadence while it is). */
bool sse_connected(void);

/* Close the stream and reset backoff (call before deep sleep). */
void sse_stop(void);

#else
static inline bool sse_pump(int b) { (void)b; return false; }
static inline bool sse_connected(void) { return false; }
static inline void sse_stop(void) { }
#endif
