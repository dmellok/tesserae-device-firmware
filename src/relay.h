/*
 * relay.h: Tesserae cloud-relay transport (remote panels).
 *
 * Lets a panel live somewhere the home instance is not reachable from. Both
 * ends talk *outbound* to a small Worker that is a per-device mailbox: home
 * seals each rendered frame and PUTs it, the panel polls and decrypts it. The
 * relay is zero-knowledge -- it holds ciphertext, both public keys and a token
 * hash, never the frame key.
 *
 * Wire contract: docs/relay/contract.md in the Tesserae repo (authoritative for
 * home, Worker and firmware alike). Crypto lives in relay_crypto.[ch] and is
 * pinned to that document's golden vectors by tools/test_relay_crypto.sh.
 *
 * Lifecycle:
 *
 *   unconfigured ──set relay_url + code──▶ pairing ──ready──▶ polling
 *                                            │                   │
 *                                            └── expired/failed ─┘ (cleared)
 *
 * Pairing may span deep sleeps, so its state lives in NVS: the panel's X25519
 * private key is kept only until the frame key is derived, then wiped.
 *
 * This is an ALTERNATIVE to the direct-REST path, not a replacement. A device
 * is either relay-paired or talking to a home server; relay_ready() decides,
 * and main.c prefers the relay when it is ready.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The hosted relay. A self-hosted Worker (packages/relay/) just sets its own
 * origin, so the two are interchangeable. */
#define RELAY_DEFAULT_URL "https://relay.tesserae.ink"

/* True when the device holds a complete mailbox identity + frame key and
 * should use the relay instead of the direct REST path. */
bool relay_ready(void);

/* True when a pairing code is set but pairing has not completed -- the caller
 * should run relay_pair_step() while the radio is up. */
bool relay_pairing_pending(void);

typedef enum {
    RELAY_PAIR_IDLE = 0,    /* nothing to do (no code, or already paired) */
    RELAY_PAIR_WAITING,     /* submitted; home has not completed it yet */
    RELAY_PAIR_DONE,        /* key derived and persisted; polling can start */
    RELAY_PAIR_EXPIRED,     /* code unknown/expired -- state cleared */
    RELAY_PAIR_ERROR,       /* transient (network/parse); safe to retry */
} relay_pair_result_t;

/* Advance the rendezvous by one step. Radio must be up.
 *
 * First call with a fresh code generates the keypair and POSTs /v1/pair;
 * subsequent calls poll GET /v1/pair/<code>. On "ready" it derives the frame
 * key from the returned home_pubkey, persists everything, and clears the code.
 * Safe to call every wake: it is a no-op once paired.
 *
 * The POST happens EXACTLY ONCE (the persisted private key is the latch),
 * because only that route enforces the code's 10-minute expiry -- re-POSTing
 * later would 404 a code that is still perfectly good for polling. The poll
 * itself has no expiry, so it may span as many wakes as home needs. */
relay_pair_result_t relay_pair_step(void);

/* Fetch the current frame from the mailbox.
 *
 * Sends If-None-Match from the cached ETag, and on 200 decrypts the sealed body
 * IN PLACE (a frame is ~1.3 MB; a second buffer would double the peak).
 *
 *   RELAY_FRAME_NEW       *frame / *len point at the decrypted image, and the
 *                         caller MUST free(*owned) once painted.
 *   RELAY_FRAME_UNCHANGED 304 -- keep what is on the glass.
 *   RELAY_FRAME_NONE      204 -- home has not published one yet.
 *   RELAY_FRAME_ERROR     network, auth, or a FAILED AUTHENTICATION TAG. A tag
 *                         failure is never painted: it means the mailbox does
 *                         not match our key, and showing it would render
 *                         whatever an attacker or a corrupted store supplied.
 *
 * The contract also exposes panel dimensions as plaintext X-Tesserae-* headers,
 * but this deliberately ignores them: the firmware knows its own geometry at
 * compile time, and validating the DECRYPTED LENGTH against it is a stronger
 * check than trusting a header the relay could have altered. */
typedef enum {
    RELAY_FRAME_NEW = 0,
    RELAY_FRAME_UNCHANGED,
    RELAY_FRAME_NONE,
    RELAY_FRAME_ERROR,
} relay_frame_result_t;

relay_frame_result_t relay_fetch_frame(uint8_t **frame, size_t *len,
                                       uint8_t **owned);

/* Commit the ETag of a frame that actually reached the glass. Kept separate
 * from the fetch so a frame that failed to decode or paint is re-fetched next
 * wake rather than skipped by a 304. */
void relay_commit_frame(void);

/* POST the device's status JSON to the mailbox (battery, RSSI, fw_version ...).
 * Body is the SAME shape a REST client posts to a home /status endpoint, so the
 * home instance can feed it through its normal heartbeat pipeline. Plaintext by
 * contract: operational telemetry, not dashboard content. Best-effort -- a
 * failure only costs the Devices UI a stale last-seen. */
bool relay_post_status(int rssi, const char *ip, uint16_t panel_w,
                       uint16_t panel_h, const char *fw_version);

/* Adopt the device config the home instance published to the config mailbox
 * (docs/relay/contract.md, "Device config" + firmware responsibility 6).
 *
 * Settings edited on the home instance reach a LOCAL device through its next
 * REST poll; a relay panel has no such channel, so without this it would keep
 * its pairing-time sleep interval and button window forever.
 *
 * Call AFTER relay_post_status() on the same wake: that response advertises the
 * current config etag, which lets this skip its request entirely when nothing
 * changed. It is still correct (just less frugal) when status was not posted --
 * it falls back to a conditional GET, which is what keeps config current on
 * wakes that skip telemetry.
 *
 * The document is sealed with the same frame key as a frame and is applied only
 * once it decrypts, authenticates and parses; the etag is stored last, so any
 * failure is retried next wake instead of being skipped by a 304 forever.
 * Every failure is non-fatal -- the last-known config simply stays in force. */
typedef enum {
    RELAY_CFG_UNCHANGED = 0,  /* 304, or status advertised the etag we hold */
    RELAY_CFG_APPLIED,        /* 200: decrypted, parsed and applied */
    RELAY_CFG_NONE,           /* 204: nothing published yet */
    RELAY_CFG_ERROR,          /* network, auth, or a failed tag; config kept */
} relay_config_result_t;

relay_config_result_t relay_sync_config(void);
