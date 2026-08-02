/*
 * relay_wire.h: pure wire handling for the cloud relay -- parsing the pairing
 * response and building mailbox URLs.
 *
 * Split out from relay.c (which is bound to esp_http_client) so the two things
 * that actually broke during bring-up are host-testable:
 *
 *   - the "ready" pairing response, which the panel must mine for install_id,
 *     device_id, device_token and home_pubkey. A missing install_id stranded
 *     pairing entirely until the relay Worker was fixed to return it.
 *   - the mailbox URL, /v1/i/<install>/d/<device>/{frame,status}. Get either id
 *     wrong and every poll 404s or, worse, addresses someone else's mailbox.
 *
 * Wire contract: docs/relay/contract.md in the Tesserae repo.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "relay_crypto.h"   /* RELAY_PUB_LEN */

typedef enum {
    RELAY_READY_PENDING = 0,  /* valid response, home has not completed it */
    RELAY_READY_OK,           /* complete: every field below is populated */
    RELAY_READY_MALFORMED,    /* unparseable, or "ready" missing a field */
} relay_ready_t;

typedef struct {
    char    install_id[64];
    char    device_id[64];
    char    device_token[256];
    uint8_t home_pub[RELAY_PUB_LEN];
} relay_pairing_t;

/* Parse a GET /v1/pair/<code> body.
 *
 * install_id is read from the TOP LEVEL, which is where the relay returns it
 * (Worker 7d7c3aa5 onward) and what the contract documents. It is also accepted
 * from inside `config` as a fallback, which costs nothing and keeps an older or
 * self-hosted Worker working.
 *
 * RELAY_READY_OK only when status == "ready" AND all four fields are present
 * and well-formed -- a partial "ready" is treated as malformed rather than
 * half-adopted, because a mailbox identity that is missing a piece produces
 * failures far from their cause. */
relay_ready_t relay_parse_ready(const char *json, size_t len,
                                relay_pairing_t *out);

/* Build the POST /v1/pair request body.
 *
 * code + pubkey_b64 are required. The rest are an OPTIONAL SELF-REPORT: the
 * panel already knows its geometry, device kind and colour gamut, so sending
 * them lets "Add a remote panel" collapse to just a device id instead of making
 * the operator retype hardware facts. Home fills in only what the pairing slot
 * left blank, so a slot value always wins.
 *
 * panel_w/h <= 0, or a NULL/empty model/gamut, omit that field entirely --
 * omitting is valid, and better than guessing: home then falls back to its
 * esp32_client default rather than being told something wrong.
 *
 * `model` is a device-KIND id (esp32_client / esp32_bw_client), NOT a hardware
 * catalog id, and `gamut` is a palette id (mono, spectra_6, gray_16, bwr_3 ...).
 * Both are wrong-answers-are-silent fields: a mono panel told esp32_client gets
 * a 4bpp Spectra packer and paints garbage, so the per-board values are taken
 * from the server's own hardware catalog (see the board headers). */
bool relay_build_pair_body(char *out, size_t cap,
                           const char *code, const char *pubkey_b64,
                           int panel_w, int panel_h,
                           const char *model, const char *gamut);

/* ---- device config (docs/relay/contract.md, "Device config") -------------
 *
 * Settings edited on the home instance reach a local device through its next
 * REST poll; a relay panel gets the same document from a config mailbox, sealed
 * with the same frame key. These two parsers are the pure half of that path.
 */

/* Read "config_etag" from a POST .../status response body.
 *
 * The status response carries the current config etag when a config document
 * exists, so a firmware that posts status learns about a config change without
 * spending an extra request. Absent (`{}`) before anything was ever published,
 * which is not an error -- false simply means "no etag advertised". */
bool relay_parse_config_etag(const char *json, size_t len,
                             char *out, size_t cap);

/* A decrypted config document. Every field is OPTIONAL: the contract says this
 * is the same object a local REST device receives in the `config` block of its
 * status response, and unknown keys are ignored. Absent is therefore "keep what
 * we have", NOT "reset to a default" -- a sentinel rather than a zero, because
 * 0 is a legitimate value for button_wake_s. */
typedef struct {
    int32_t sleep_interval_s;   /* -1 when absent */
    int32_t button_wake_s;      /* -1 when absent */
    int8_t  always_on;          /* -1 when absent, else 0/1 */
} relay_devcfg_t;

/* Parse a decrypted config document. False only if the body is not a JSON
 * object at all; a valid object with no recognised keys parses fine and leaves
 * every field at its absent sentinel. always_on accepts a bool or 0/1, matching
 * how the REST path reads the same field. */
bool relay_parse_config(const char *json, size_t len, relay_devcfg_t *out);

/* Build "<base>/v1/i/<install>/d/<device>/<leaf>" (leaf: "frame" or "status").
 * False if any part is empty or the result would not fit, so a truncated URL
 * can never be silently requested. */
bool relay_mailbox_url(char *out, size_t cap, const char *base,
                       const char *install_id, const char *device_id,
                       const char *leaf);
