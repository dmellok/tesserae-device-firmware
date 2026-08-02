/*
 * HTTP(S) image downloader.
 *
 * Streams the response body into a PSRAM-allocated buffer (returned to
 * the caller, who must free() it). Uses the mbedTLS root-CA bundle, so
 * any public HTTPS URL works without per-host certs.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t *data;       /* PSRAM allocation; caller must free() */
    size_t   len;
    char     content_type[64];  /* e.g. "image/jpeg", "application/octet-stream" */
} fetched_image_t;

/* Refuse anything larger than this. A panel-native raw frame is 960KB;
 * a generous JPEG cap of ~4MB leaves headroom while preventing a runaway
 * download from OOM'ing us. */
#define IMAGE_FETCH_MAX_BYTES  (4 * 1024 * 1024)

/* On success, fills `out` (caller frees out->data). On failure leaves
 * out->data == NULL. */
esp_err_t image_fetch(const char *url, fetched_image_t *out);

/* As image_fetch(), adding an Authorization: Bearer header when bearer_token
 * is non-empty (deck frame endpoint). Returns ESP_ERR_NOT_FOUND on a 404 --
 * the deck contract's "stale manifest, re-fetch it" signal. */
esp_err_t image_fetch_auth(const char *url, const char *bearer_token,
                           fetched_image_t *out);

/* As image_fetch_auth(), plus conditional-GET support for the cloud relay:
 * sends If-None-Match when etag_in is non-empty, reports the raw HTTP status
 * in *status, and copies the response ETag (quotes stripped) into etag_out.
 *
 * The relay leans on all three: 304 means "unchanged, keep the current image"
 * and 204 "no frame yet", neither of which is an error, and the returned ETag
 * is what the next poll sends back. Those statuses come back ESP_OK with
 * out->data == NULL, so the caller branches on *status rather than on err.
 * etag_out may be NULL if the caller does not track it. */
esp_err_t image_fetch_conditional(const char *url, const char *bearer_token,
                                  const char *etag_in,
                                  char *etag_out, size_t etag_out_cap,
                                  int *status, fetched_image_t *out);
