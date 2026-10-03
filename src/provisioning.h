/*
 * Settings provisioning over HTTP.
 *
 * Two entry points share the same form (WiFi creds + MQTT broker + device_id):
 *
 *   provisioning_run_blocking()      -- first-boot / no-creds path. Brings up
 *       a SoftAP + wildcard DNS responder so phones auto-trigger their "sign
 *       in to network" prompt, then serves the form.
 *
 *   settings_server_run_blocking()   -- always-on editor. Assumes STA is
 *       already connected; serves the same form on the LAN IP and advertises
 *       it over mDNS at http://tesserae-<device_id>.local/.
 *
 * Both block until the user submits (settings persisted to NVS) or the
 * PROVISION_PORTAL_TIMEOUT_S timeout fires, then tear everything down.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include "setup_fields.h"

typedef enum {
    PROVISIONING_RESULT_SAVED = 0,
    PROVISIONING_RESULT_TIMEOUT,
    PROVISIONING_RESULT_BLE_REQUESTED,
} provisioning_result_t;

/* Captive portal (SoftAP). It may finish because settings were saved, the idle
 * timeout elapsed, or the board's maintenance key requested Companion BLE. */
provisioning_result_t provisioning_run_blocking(void);

/* Split form of the captive portal so a slow panel splash can be painted with
 * the AP already live: provisioning_begin() brings up the SoftAP + DNS + HTTP
 * (non-blocking, joinable in ~1-2 s), then provisioning_serve() blocks until a
 * save, a physical BLE switch request, or the idle timeout and tears everything
 * down (same return contract as provisioning_run_blocking(), which is now just
 * begin()+serve()). */
void                  provisioning_begin(void);
provisioning_result_t provisioning_serve(void);

/* Always-on LAN settings editor (STA must already be up). Same return
 * contract as provisioning_run_blocking(). */
esp_err_t settings_server_run_blocking(void);

/* Saves a checked set of fields (setup_fields_check) the way the portal form
 * does: Wi-Fi, then the transport for the chosen mode, dropping the token and
 * cached frame when the server changes so the panel re-onboards cleanly.
 * ESP_ERR_INVALID_STATE when the Wi-Fi write failed, another error for the
 * Tesserae settings. Used by the form, the USB serial setup and BLE. */
esp_err_t provisioning_apply(const setup_fields_t *f);

/* The portal's scan of nearby networks as a JSON array of {ssid,rssi,secure};
 * "[]" when no portal scan has run. */
void provisioning_scan_json(char *out, size_t cap);

/* True while the captive portal or the LAN settings editor is serving. */
bool provisioning_portal_open(void);

/* Settings were saved by another path (serial) while the portal is open:
 * finish the portal as after a form post, so the board restarts. False when
 * no portal is open. */
bool provisioning_notify_saved(void);
