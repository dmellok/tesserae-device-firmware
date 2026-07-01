/*
 * Wake-time heartbeat: battery, signal strength, IP.
 *
 * Builds a compact JSON object suitable for publishing to a retained
 * MQTT topic so a companion dashboard can see "last known device state"
 * even while the device is asleep.
 */
#pragma once

#include <stddef.h>
#include <time.h>

#include "esp_system.h"   /* esp_reset_reason_t */

/* Fill `dst` with a JSON document like:
 *   {"battery_mv":3950,"battery_pct":67,"rssi":-45,
 *    "ip":"192.168.50.234","fw_version":"0.6.0",
 *    "kind":"esp32_client","panel_w":1200,"panel_h":1600,
 *    "sleep_interval_s":900,"next_sleep_s":900,
 *    "wake_reason":"timer","sleep_until":1759264800}
 *
 * `sleep_interval_s` is the device's *configured* deep-sleep duration
 * (for diagnostics). `next_sleep_s` is the deep-sleep duration the device
 * is about to enter -- usually identical, but firmwares that adjust their
 * own cadence can differ. Both feed Tesserae's smart-sync scheduler
 * (https://github.com/dmellok/tesserae/issues/10).
 *
 * `sleep_until` is the absolute unix timestamp (UTC seconds) the device
 * intends to wake. Pass 0 if NTP isn't synced (the firmware can't compute
 * it) and the field is omitted from the JSON; the server falls back to
 * computing the wake time from heartbeat-receive-time + next_sleep_s.
 *
 * `wake_reason` is a short string mapping of the esp_reset_reason_t for
 * this wake.
 *
 * Never fails -- unknown fields are emitted as 0 / empty string. The
 * caller's buffer should be at least 320 bytes; smaller is safe but
 * fields may be truncated. */
void heartbeat_format_json(char *dst, size_t dst_sz,
                           int sleep_interval_s,
                           esp_reset_reason_t reset_reason,
                           time_t sleep_until);
