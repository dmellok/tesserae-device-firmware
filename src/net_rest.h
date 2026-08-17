/*
 * net_rest.h: Tesserae REST API client (replaces the MQTT transport).
 *
 * Talks to the Tesserae control endpoints under <server_url>/api/v1/device/ .
 * Built on esp_http_client, which (unlike a bare MQTT/httpc path) gives us
 * request headers (Authorization, If-None-Match, X-Pairing-Code), the response
 * status code, and response headers (ETag, Retry-After, Date). The large frame
 * .bin is still fetched with image_fetcher's image_fetch().
 *
 * Identity (device_id, server_url, token, pairing_code, etag) is read from
 * rest_config; per-cycle values (panel size, mac, rssi, ip) are passed in. One
 * request in flight at a time. Ported from tesserae-device-pico-bin/net_rest.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"   /* BOARD_HAS_TOUCH gates the touch fields below */
#include "deck.h"         /* DECK_VERSION_CAP (deck resync signal) */
#include "frame_collection.h"
#if TESSERAE_OTA_CAPABILITY_ENABLED
#include "ota_manifest.h"
#endif

/* Outcome of a REST call, mapping the HTTP statuses the cycle reacts to. */
typedef enum {
    REST_OK = 0,          /* 200 / 201 */
    REST_NOT_MODIFIED,    /* 304 (frame unchanged) */
    REST_NO_CONTENT,      /* 204 (nothing rendered server-side yet) */
    REST_UNAUTH,          /* 401 (token invalid/revoked) */
    REST_FORBIDDEN,       /* 403 (pairing code / device mismatch) */
    REST_RATELIMIT,       /* 429 (retry_after_s populated) */
    REST_NOT_FOUND,       /* 404 (overlay contract: feature off / stale digest) */
    REST_HTTP_ERR,        /* other 4xx/5xx */
    REST_NET_ERR,         /* DNS / TCP / timeout */
} rest_status_t;

typedef struct {
    bool     registered;        /* admin clicked Register: token is present */
    char     token[256];        /* device_token, when registered */
    char     device_id[33];     /* canonical id the server matched us to (MAC) */
    int32_t  sleep_interval_s;  /* from config object, -1 if absent */
    uint32_t server_time;       /* unix seconds, 0 if absent */
    int      retry_after_s;     /* how long to wait before the next discover */
} rest_discover_out_t;

typedef struct {
    char     token[256];
    char     device_id[33];
    int32_t  sleep_interval_s;
    int32_t  button_wake_s;     /* config.button_wake_s: -1 if absent */
    uint32_t server_time;
    int      retry_after_s;
} rest_register_out_t;

typedef struct {
    char     url[256];          /* frame .bin URL (may be relative) */
    char     format[16];        /* e.g. "bin" */
    uint16_t panel_w, panel_h;
    char     etag[80];          /* new ETag to persist (quotes stripped) */
    int32_t  button_wake_s;     /* top-level "button_wake_s": -1 if absent */
    /* Protocol v2: optional "manifest" block. Present only on v2 servers;
     * its absence on a 200 is the primary v1-fallback signal (proto2 §10). */
    bool     has_manifest;
    char     manifest_digest[17];
    char     manifest_url[160];
    /* Touch v3: the frame's LAYOUT digest, stable across data-only redraws (a
     * clock tick does not invalidate it). Accepted top-level or inside a "touch"
     * block. The server does not send it yet, so "" is the normal case and is
     * NOT a v3-off signal -- only a 404/204 from /frame/spec is. When present it
     * is purely an optimisation: matching the held digest lets touch3 skip the
     * spec pull for a data-only redraw (touch3.h). */
    char     layout_digest[33];
} rest_frame_out_t;

typedef struct {
    int32_t  next_poll_s;       /* deep-sleep duration to use, -1 if absent */
    int32_t  sleep_interval_s;  /* from config object, -1 if absent */
    int32_t  button_wake_s;     /* config.button_wake_s: -1 if absent */
    uint32_t server_time;       /* unix seconds, 0 if absent */
    int      retry_after_s;     /* set on REST_RATELIMIT */
#if BOARD_HAS_TOUCH
    int      touch_enabled;     /* config.touch_enabled: -1 absent, else 0/1 */
    int32_t  touch_linger_s;    /* config.touch_linger_s: -1 absent */
#endif
#if TESSERAE_OTA_CAPABILITY_ENABLED
    bool     ota_present;       /* response contained a top-level ota field */
    ota_verify_reason_t ota_reason;
    ota_manifest_t ota_manifest; /* populated only when ota_reason == OK */
#endif
    bool     deck_present;      /* response carried "deck": {"version"} */
    char     deck_version[DECK_VERSION_CAP];
    bool     collection_present; /* response carried collection id/kind/version */
    char     collection_id[FC_ID_CAP];
    char     collection_kind[FC_KIND_CAP];
    char     collection_version[FC_VERSION_CAP];
#if BOARD_OVERLAY_PARTIAL
    /* Raw "overlay_values" object from the response, "" when absent. Same
     * semantics as the polled values document (overlay.h); newest seq wins. */
    char     overlay_values[512];
    /* Raw "overlay_patches" object (schema 2), "" when absent. Sized for the
     * contract cap: 12 rects + header comfortably fits. */
    char     overlay_patches[1536];
    /* Raw "sync" object (proto v2: frame/manifest/bundle digest triple),
     * "" when absent. Same envelope as the SSE sync event. */
    char     sync_obj[256];
    /* Clock discipline for local: text keys; -1 when absent. */
    int      local_hh, local_mm;
#endif
} rest_status_out_t;

/* POST /api/v1/device/discover (unauthenticated). Zero-touch onboarding: the
 * admin claims the device by clicking Register in the Tesserae UI; the next
 * discover returns the token by MAC match. On REST_OK inspect out->registered
 * (true = token claimed; false = waiting on the admin, sleep out->retry_after_s). */
rest_status_t rest_discover(uint16_t panel_w, uint16_t panel_h,
                            const char *mac, const char *fw_version,
                            rest_discover_out_t *out, uint32_t timeout_ms);

/* POST /api/v1/device/register with the X-Pairing-Code header (strict gating).
 * Idempotent server-side. On REST_OK, out->token holds the device token. */
rest_status_t rest_register(uint16_t panel_w, uint16_t panel_h,
                            const char *mac, const char *fw_version,
                            rest_register_out_t *out, uint32_t timeout_ms);

/* Validate a staged server URL without changing rest_config. Requires a 200
 * Companion capability response whose product is tesserae and API name is
 * companion. Used by BLE setup before committing Wi-Fi or server settings. */
bool rest_probe_server_url(const char *server_url, uint32_t timeout_ms);

/* Report a front-button press with the subsequent frame request.
 * name is "refresh"/"left"/"right" (see buttons.h); NULL/"" clears it. event_id
 * is a monotonic per-press id the server can dedup on. Adds
 * ?button=<name>&button_event_id=<id> to the frame GET. An acknowledged frame
 * response clears it before /status; a pre-ack failure retains it so the status
 * body can deliver {"button","button_event_id"} as a fallback. */
void rest_set_button(const char *name, uint64_t event_id);

/* Read the press still pending delivery, if any.
 *
 * Exists so the RELAY transport can report the same press with the same event
 * id as the REST path would. A relay panel's frame GET terminates at the relay,
 * so the ?button= query contract cannot reach home; the press rides the status
 * body instead (docs/relay/contract.md, "Buttons over the relay"). Reading the
 * one pending report -- rather than relay.c keeping its own -- is what makes
 * "same counter, and the id never changes within a wake" true by construction
 * instead of by convention. */
bool rest_pending_button(char *name, size_t cap, uint64_t *event_id);

#if BOARD_HAS_TOUCH
/* Report a touch stroke with the subsequent frame GET (v1 fallback path).
 * Coordinates are in the served frame's pixel space; x1/y1 is the stroke end
 * (== start for a point tap). digest is the ETag currently displayed (quotes
 * stripped). event_id shares the button wake-event counter and dedups
 * retries. Clear with rest_set_touch(0,0,0,0,0,NULL,0) (a zero digest
 * disables the params). Sticky until cleared. A stale digest or miss
 * degrades server-side to a plain poll. */
void rest_set_touch(int x0, int y0, int x1, int y1, uint32_t ms,
                    const char *digest, uint64_t event_id);
#endif

/* GET /api/v1/device/<id>/frame with Bearer auth and If-None-Match (cached
 * etag). REST_OK fills out (incl. the new etag); REST_NOT_MODIFIED and
 * REST_NO_CONTENT mean skip the paint. */
rest_status_t rest_get_frame(rest_frame_out_t *out, uint32_t timeout_ms);

/* POST /api/v1/device/<id>/status with Bearer auth. Reports telemetry and
 * reads back next_poll_s / config / server_time. */
rest_status_t rest_post_status(int rssi, const char *ip,
                               uint16_t panel_w, uint16_t panel_h,
                               int32_t next_sleep_s, uint32_t sleep_until,
                               const char *fw_version,
                               rest_status_out_t *out, uint32_t timeout_ms);

/* ---- deck cache (SD card; see deck.h / deck_cache.h) ---- */

/* Advertise the deck_cache capability with the next register/status bodies.
 * capacity_bytes is the card's free space (0 = no card mounted -> the
 * capability object is omitted and the wire bodies are byte-identical to a
 * cacheless build). Set once per wake, after sdcard_mount(). */
void rest_set_deck_capability(uint64_t capacity_bytes);

/* Report that the displayed frame came from the SD cache: adds deck_page_id +
 * deck_version to the next /status body and to a same-wake /frame query.
 * NULL/"" clears. Sticky for the wake. */
void rest_set_deck_painted(const char *page_id, const char *version);

/* GET /api/v1/device/<id>/deck (Bearer). REST_OK: the manifest JSON is copied
 * into buf (NUL-terminated, *out_len set) for deck_manifest_parse().
 * REST_NO_CONTENT: no deck bound. */
rest_status_t rest_get_deck_manifest(char *buf, size_t cap, size_t *out_len,
                                     uint32_t timeout_ms);

/* Compose the authorised deck frame URL for image_fetch_auth(). */
void rest_deck_frame_url(const char *digest, char *out, size_t cap);

/* The raw bearer token (for image_fetch_auth on deck frame downloads). */
const char *rest_bearer_token(void);

/* ---- producer-neutral frame-cache collections (offline Albums) ---- */

/* Advertise frame_cache beside deck_cache while usable SD storage is mounted. */
void rest_set_frame_cache_capability(uint64_t capacity_bytes, uint16_t max_frames);

/* Report truthful collection state on /status. NULL/empty id clears it. */
void rest_set_collection_report(const char *id, const char *version,
                                uint16_t cached, uint32_t total,
                                const char *state);

/* GET /api/v1/device/<id>/collection. REST_NO_CONTENT means unbound. */
rest_status_t rest_get_collection_manifest(char *buf, size_t cap,
                                           size_t *out_len,
                                           uint32_t timeout_ms);

/* ---- overlay render mode (overlay.h; boards with partial refresh) ---- */

/* GET /api/v1/device/<id>/frame/overlay/<digest>. REST_OK copies the spec
 * JSON into buf (NUL-terminated). REST_NOT_FOUND = no overlay for this
 * frame / server predates the feature (dormant). */
rest_status_t rest_get_overlay_spec(const char *digest, char *buf, size_t cap,
                                    size_t *out_len, uint32_t timeout_ms);

/* GET /api/v1/device/<id>/frame/data?digest=<digest> (values document). */
rest_status_t rest_get_frame_data(const char *digest, char *buf, size_t cap,
                                  size_t *out_len, uint32_t timeout_ms);

/* ---- protocol v2 (proto2.h; device-owned touch) ---- */

/* GET /api/v1/device/<id>/frame/manifest?digest=<digest>. REST_OK copies the
 * interaction-manifest JSON into buf (NUL-terminated). REST_NOT_FOUND = v1
 * server or no interactivity on this frame (v1 fallback, proto2 §10). */
rest_status_t rest_get_manifest(const char *digest, char *buf, size_t cap,
                                size_t *out_len, uint32_t timeout_ms);

/* GET /api/v1/device/<id>/bundle (state bundle manifest). REST_NOT_FOUND /
 * REST_NO_CONTENT = no bundle; all nav is tier 2. */
rest_status_t rest_get_bundle(char *buf, size_t cap, size_t *out_len,
                              uint32_t timeout_ms);

/* ---- touch v3 (touch3.h; device-owned touch primitives) ---- */

/* GET /api/v1/device/<id>/frame/spec?layout=<layout_digest>. REST_OK copies the
 * touch spec JSON into buf (NUL-terminated): { layout_digest, primitives[] },
 * with atlases[] absent until the server's atlas pipeline lands.
 *
 * The ?layout= param is ADVISORY -- the server returns the spec for the device's
 * CURRENT frame whatever is passed -- so the caller detects a layout change by
 * comparing the RETURNED layout_digest against what it holds, not by trusting
 * the query. REST_NOT_FOUND / REST_NO_CONTENT = "no touch for this frame":
 * render the image only (touch-v3 firmware-spec §5). */
rest_status_t rest_get_frame_spec(const char *layout_digest, char *buf,
                                  size_t cap, size_t *out_len,
                                  uint32_t timeout_ms);

/* What an /interact reply carries.
 *
 * `outcome` is always present on a 200 (ha_dispatched, dispatched, fetched,
 * webhook_dispatched, noop, no_target, no_frame, deduped). It is for LOGGING
 * ONLY -- never branch on it: the device already drew local feedback before the
 * request went out, so there is nothing for an outcome to change.
 *
 * The confirmed-state fields are plumbing for a server capability that does NOT
 * exist yet (as of the 2026-07-27 server sync the reply is only
 * {outcome, primitive_id}); confirmed switch/slider state is to arrive on the
 * SSE values channel instead. They stay false until then, which the caller must
 * read as "still optimistic", NOT as "confirmed unchanged". */
typedef struct {
    char  outcome[24];     /* "" when the reply carried none */
    bool  have_state;      /* switch: confirmed on/off */
    bool  state;
    bool  have_value;      /* slider/stepper: confirmed value */
    float value;
} rest_interact_out_t;

/* POST /api/v1/device/<id>/interact -- the v3 semantic event report:
 *   { primitive_id, interaction: "tap"|"set", value?, layout_digest, event_id }
 * value is passed only when has_value (steppers/sliders). Fire-and-forget for
 * feedback purposes: local feedback is already on glass before this runs.
 * out may be NULL when the caller does not reconcile. */
rest_status_t rest_post_interact(const char *primitive_id,
                                 const char *interaction,
                                 bool has_value, float value,
                                 const char *layout_digest, uint64_t event_id,
                                 rest_interact_out_t *out,
                                 uint32_t timeout_ms);

/* POST /api/v1/device/<id>/tap: the v2 action report. gesture is a
 * p2_gesture_name() string; value is 0-100 for slides, -1 to omit; digest is
 * the frame hit-tested against. outcome (cap >= 24) receives the server's
 * "outcome" string on REST_OK ("" when absent). Fire-and-forget for feedback
 * purposes: the caller applies local feedback before, and regardless of,
 * this call's result. */
rest_status_t rest_post_tap(const char *region_id, const char *gesture,
                            int value, const char *digest, uint64_t event_id,
                            int x0, int y0, int x1, int y1,
                            char *outcome, size_t outcome_cap,
                            uint32_t timeout_ms);
