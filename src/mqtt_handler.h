/*
 * Single-shot MQTT job grabber.
 *
 * Each wake-up:
 *   1. connects to the broker
 *   2. subscribes to the update topic
 *   3. waits MQTT_WAIT_MS for the retained message
 *   4. returns the URL string (and any options) it carries
 *   5. tears down
 *
 * The Python listener publishes its job either as a bare URL string or
 * as a JSON object with a "url" field; we accept both, since either is
 * a one-line change on the publisher side.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char url[256];     /* zero-terminated; empty if no message arrived */
} mqtt_job_t;

/* Blocks for up to MQTT_WAIT_MS. Returns ESP_OK if a job was captured
 * (job->url is non-empty), ESP_ERR_TIMEOUT if the broker had no retained
 * message, or another esp_err_t on transport failure.
 *
 * Does NOT publish the heartbeat -- that's a separate call (mqtt_publish_status
 * below), invoked at the END of the wake so sleep_until is wall-clock-accurate
 * for the server's smart-sync scheduler. */
esp_err_t mqtt_fetch_retained(mqtt_job_t *job);

/* One-shot: bring up MQTT, publish `payload` retained QoS 1 to
 * tesserae/<device_id>/status, wait for the broker PUBACK, tear down.
 *
 * The caller is expected to have WiFi up. Used at the END of every wake so
 * the embedded next_sleep_s / sleep_until reflect the sleep about to start
 * (per Tesserae's smart-sync wire contract). */
esp_err_t mqtt_publish_status(const char *payload);
