/*
 * NVS-backed MQTT settings.
 *
 * Provisioning (the captive portal) writes these on first boot, mqtt_handler
 * reads them on every wake. Any field missing from NVS falls back to the
 * compile-time defaults in app_config.h.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char uri[160];        /* mqtt://host:port or mqtts://host:port */
    char device_id[33];   /* topic prefix: tesserae/<device_id>/... */
    char user[64];        /* empty if broker is open */
    char pass[64];        /* empty if broker is open */
} mqtt_config_t;

/* Fill `out` from NVS, with defaults from app_config.h for any missing key.
 * Migrates the legacy `mqtt/topic` key into `device_id` on first read.
 * Never fails -- worst case is all defaults. */
void mqtt_config_load(mqtt_config_t *out);

/* Validate `device_id` (2-32 chars, [a-z0-9_-], leading letter) against the
 * same rules Tesserae enforces. Returns true if acceptable. */
bool mqtt_device_id_valid(const char *device_id);

/* Persist all four fields. The URI gets the mqtt:// scheme prepended if it's
 * missing one. Returns ESP_ERR_INVALID_ARG if uri is empty or device_id is
 * malformed (see mqtt_device_id_valid). `pass == NULL` keeps the stored
 * broker password unchanged; `pass == ""` clears it. */
esp_err_t mqtt_config_save(const char *uri, const char *device_id,
                           const char *user, const char *pass);
