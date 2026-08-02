/*
 * collection_run.h: offline Album wake-loop orchestration.
 *
 * The network heartbeat and local Album cadence have separate RTC deadlines.
 * Timer wakes before the heartbeat may paint from SD and return to sleep with
 * the radio off; button/touch/manual wakes always continue through the normal
 * server cycle. Assignment/version changes are learned from /status and synced
 * after the current network frame has been painted.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void collection_boot(void);

/* On a timer wake, serve a due Album frame and/or decide the network heartbeat
 * is not due yet. True means main may go directly back to sleep without WiFi. */
bool collection_try_local_cycle(void);

/* Consume the collection envelope from a successful /status response. Returns
 * true when a manifest/frame sync tail is required this wake. */
bool collection_sync_pending(bool present, const char *id, const char *kind,
                             const char *version);

/* Fetch/validate/diff/cache the announced collection while WiFi is still up. */
void collection_sync_tail(bool present, const char *id, const char *kind,
                          const char *version);

/* A successful status heartbeat resets only the network deadline. */
void collection_network_polled(int32_t normal_poll_s);

/* A direct network frame is an interruption: give it a full Album interval on
 * glass, then resume. digest may identify an Album frame, or may be NULL. */
void collection_network_painted(const char *digest);

/* Shorten the ordinary sleep to the next local frame/network deadline. */
int32_t collection_next_sleep_s(int32_t ordinary_s);
