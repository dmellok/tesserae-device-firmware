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

/* Captive portal (SoftAP). ESP_OK if settings were saved, ESP_ERR_TIMEOUT
 * if it timed out with no submission. */
esp_err_t provisioning_run_blocking(void);

/* Split form of the captive portal so a slow panel splash can be painted with
 * the AP already live: provisioning_begin() brings up the SoftAP + DNS + HTTP
 * (non-blocking, joinable in ~1-2 s), then provisioning_serve() blocks until a
 * save or the idle timeout and tears everything down (same return contract as
 * provisioning_run_blocking(), which is now just begin()+serve()). */
void      provisioning_begin(void);
esp_err_t provisioning_serve(void);

/* Always-on LAN settings editor (STA must already be up). Same return
 * contract as provisioning_run_blocking(). */
esp_err_t settings_server_run_blocking(void);
