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

/* Outcome of a REST call, mapping the HTTP statuses the cycle reacts to. */
typedef enum {
    REST_OK = 0,          /* 200 / 201 */
    REST_NOT_MODIFIED,    /* 304 (frame unchanged) */
    REST_NO_CONTENT,      /* 204 (nothing rendered server-side yet) */
    REST_UNAUTH,          /* 401 (token invalid/revoked) */
    REST_FORBIDDEN,       /* 403 (pairing code / device mismatch) */
    REST_RATELIMIT,       /* 429 (retry_after_s populated) */
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

/* Report a front-button press with the subsequent frame request.
 * name is "refresh"/"left"/"right" (see buttons.h); NULL/"" clears it. event_id
 * is a monotonic per-press id the server can dedup on. Adds
 * ?button=<name>&button_event_id=<id> to the frame GET. An acknowledged frame
 * response clears it before /status; a pre-ack failure retains it so the status
 * body can deliver {"button","button_event_id"} as a fallback. */
void rest_set_button(const char *name, uint32_t event_id);

#if BOARD_HAS_TOUCH
/* Report a touch stroke with the subsequent frame GET. Coordinates are in the
 * served frame's pixel space; x1/y1 is the stroke end (== start for a point
 * tap). digest is the ETag currently displayed (quotes stripped). event_id
 * shares the button wake-event counter and dedups retries. Clear with
 * rest_set_touch(0,0,0,0,0,NULL,0) (a zero digest disables the params). Sticky
 * until cleared. A stale digest or miss degrades server-side to a plain poll. */
void rest_set_touch(int x0, int y0, int x1, int y1, uint32_t ms,
                    const char *digest, uint32_t event_id);
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
