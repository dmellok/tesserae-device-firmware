/*
 * log_capture.h: keep the device's own log so it can be uploaded to the
 * server it talks to (device log upload plan, Protocol v1).
 *
 * Every esp_log line still goes to the UART exactly as before. A copy, with
 * ANSI colour codes stripped and URL query strings and bearer tokens redacted
 * (log_ring.h), is also written to:
 *   - a ring in RTC_NOINIT memory (LOG_CAPTURE_RING_BYTES, default 3072),
 *     written continuously, so the tail of the previous wake (its paint and
 *     its sleep entry) is still there on the next one. Survives deep sleep,
 *     software reset and panic; lost on power-off;
 *   - a wake buffer in PSRAM (32 KB, keeping the newest bytes) holding this
 *     boot. Boards without PSRAM get none unless their header sets
 *     LOG_CAPTURE_WAKE_INTERNAL_BYTES; they upload from the RTC ring instead.
 * Each boot starts with a "# tesserae-log v1 ..." header line.
 *
 * Nothing is uploaded unless the server asks: a /status response carrying
 * "logs": {"upload": true} makes the caller run log_capture_upload() before
 * the radio goes down. The status body advertises the capability on every
 * beat (log_capture_ring_bytes()).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Validate the RTC ring, set aside its unsent bytes from before this boot,
 * allocate the wake buffer, write the header and install the esp_log hook.
 * Call once, as early in app_main as practical. */
void log_capture_init(void);

/* Capacity of the RTC ring, advertised as logs.ring_bytes. */
uint32_t log_capture_ring_bytes(void);

/* POST everything not yet uploaded (unsent tail of earlier boots, then this
 * boot so far, front-truncated to 64 KB) to <server>/api/v1/device/<id>/log.
 * On a 2xx the bytes are marked uploaded and never sent again; on anything
 * else they stay eligible for the next request. No retry. A panel without a
 * home server URL (relay only) never uploads. Returns true on a 2xx. */
bool log_capture_upload(void);
