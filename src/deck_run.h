/*
 * deck_run.h: wake-loop orchestration for the SD deck cache.
 *
 * Ties together sdcard (mount), deck_cache (files), deck (pure logic),
 * rest_config (persisted nav state; the RTC copy here survives deep sleep)
 * and net_rest (capability + SD-paint reporting). main.c calls exactly five
 * hooks; every one degrades to a no-op when no card is mounted, so a
 * cardless device or a deck-unaware server sees today's behaviour bit for
 * bit. See the deck contract in deck.h.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "net_rest.h"   /* rest_status_out_t (deck resync signal) */

/* Boot hook, call once after rest_config_load(): probes/mounts the card,
 * advertises the deck_cache capability for this wake's register/status
 * bodies, restores nav state (RTC copy wins over the NVS mirror), and arms
 * the deck_page_id/deck_version report when the displayed frame came from
 * the cache. */
void deck_boot(void);

/* Local nav for the wake's button press. Returns false when the deck cannot
 * serve it (no card / no synced deck / no matching link / frame missing,
 * stale or corrupt): caller proceeds with today's network path (the button
 * report is already pending). Returns true when the press was painted from
 * SD; then *fallthrough tells the caller what happened during the local
 * button_wake_s window: false = window closed cleanly, go to sleep WITHOUT
 * bringing the radio up; true = a later press had no local link, its report
 * has been armed via rest_set_button(), continue into the network cycle.
 * event_seq is main's wake-event counter (shared with touch). */
bool deck_try_button(const char *button, uint32_t *event_seq, bool *fallthrough);

/* Local nav for a captured touch tap: hit-tests (x, y) -- the stroke end --
 * against the current page's zones. True = painted from SD; the caller must
 * clear the pending rest_set_touch() report and skip the network cycle.
 * False = proceed exactly as today. */
bool deck_try_touch(int x, int y);

/* A network frame was just painted: the SD-paint report no longer describes
 * the display. Clears the persisted flag and this wake's report fields. */
void deck_network_painted(void);

/* True when this wake's /status response asks for a cache resync (version
 * differs from what is synced on card). deck_present/version come from the
 * status response's "deck" object. Decides whether main keeps the radio up
 * through the paint so the sync tail can run. */
bool deck_sync_pending(bool deck_present, const char *version);

/* Tail-of-wake sync (radio must still be up): fetch the manifest, diff
 * digests, fetch only missing frames, delete orphans; on a 204 drop local
 * nav state but keep the cached files. Persists nav/version state itself. */
void deck_sync_tail(bool deck_present, const char *version);

/* Pre-sleep hook: unmount + power down the card. */
void deck_pre_sleep(void);
