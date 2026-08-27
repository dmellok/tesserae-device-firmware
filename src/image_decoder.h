/*
 * Image-payload -> panel-native frame buffer.
 *
 * v1 accepts exactly one wire format: a 960000-byte raw 4-bpp packed buffer
 * matching the Waveshare 13.3" Spectra 6 panel layout. The Tesserae server's
 * esp32_bin renderer produces this directly; anything else is treated as a
 * server-side bug and rejected without painting.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "image_fetcher.h"

/* Produce an EPD_BUF_BYTES-sized panel frame. On success the download buffer
 * is handed over: *out_frame takes src->data (src->data is NULLed) and the
 * caller owns it and must free() it. Callers' existing free(src->data) after
 * the call stays correct either way. Hints (url, content_type) come from
 * image_fetch(); both are advisory. */
esp_err_t image_decode_to_frame(fetched_image_t *src,
                                const char *url,
                                uint8_t **out_frame);
