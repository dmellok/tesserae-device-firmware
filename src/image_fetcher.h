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
