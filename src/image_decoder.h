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

/* Allocate (in PSRAM) and produce an EPD_BUF_BYTES-sized panel frame.
 * On success, *out_frame is owned by the caller and must be free()d.
 * Hints (url, content_type) come from image_fetch(); both are advisory. */
esp_err_t image_decode_to_frame(const fetched_image_t *src,
                                const char *url,
                                uint8_t **out_frame);
