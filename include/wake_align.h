/* Synchronized wake: absolute wake targets + RTC drift correction.
 *
 * The server's /status response may carry "wake_at", the absolute unix
 * second it wants the next check-in to land on (wake alignment: the whole
 * fleet paints on a shared wall-clock grid). Relative next_poll_s alone
 * lands late by however long the frame fetch + panel refresh took after
 * the response was received, because the countdown only starts at deep
 * sleep entry. Converting the absolute target to a delta at the moment of
 * sleep sheds that slip entirely; the wall clock is trustworthy there
 * because every HTTP response disciplines it from the server's Date
 * header (net_rest.c), seconds before the sleep decision.
 *
 * The remaining error is the RTC slow clock's own drift over the sleep
 * (internal RC oscillator, ~1-2% typical). Since the clock is
 * re-disciplined every wake, the correction falls out for free: the
 * settimeofday adjustment measured against the time elapsed since the
 * previous discipline IS the drift ratio, smoothed here into an EWMA
 * (kept in RTC RAM) and applied when programming the sleep timer. */
#pragma once

#include <stdint.h>

/* Record the one-shot absolute wake target from the latest /status
 * response ("wake_at"); 0 = none. RAM only, never persisted: the value
 * is only meaningful for the sleep it was issued for. */
void wake_align_set_target(uint32_t epoch);

/* The sleep duration to actually use: seconds until the recorded target,
 * or fallback_s when there is no target, the clock is not sane, the
 * target has effectively passed, or something else (collection playback,
 * a projected content change) wants an earlier wake than the target. */
int wake_align_sleep_s(int fallback_s);

/* Called by net_rest.c whenever the wall clock is disciplined from a
 * server Date header: local clock (ms) just before the set, server time
 * (ms) it was set to. Feeds the drift EWMA. */
void wake_align_note_discipline(int64_t local_ms, int64_t server_ms);

/* Drift-corrected timer program for a sleep of sleep_s seconds. Without
 * an established drift estimate this is exactly sleep_s * 1e6. */
uint64_t wake_align_timer_us(int sleep_s);
