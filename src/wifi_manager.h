/*
 * WiFi credential persistence + STA connect.
 *
 * NVS layout (namespace "wifi"):
 *   ssid  : string (max 32)
 *   pass  : string (max 64, may be empty for open networks)
 *
 * The provisioning flow writes here; main.c reads + tries to connect.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Initialise NVS + esp-netif + esp-event. Idempotent. */
esp_err_t wifi_manager_init(void);

/* True iff non-empty SSID is stored. */
bool wifi_creds_present(void);

/* Copy the effective SSID (stored, else compile-time default) into `out`.
 * Returns false and leaves `out` empty if neither is set. */
bool wifi_creds_get_ssid(char *out, size_t out_sz);

/* Write the current STA IPv4 (e.g. "192.168.50.234") into `out`. Returns
 * false and leaves `out` empty if the STA interface has no IP yet. */
bool wifi_manager_get_sta_ip(char *out, size_t out_sz);

/* Block until the STA holds a ROUTABLE (global or unique-local) IPv6 address,
 * or the timeout passes. Returns immediately once one exists. Used by the REST
 * layer before the first request of a wake when the server host is only
 * reachable over IPv6 (issue #2) -- SLAAC needs a couple of seconds after
 * association, longer than the v4 DHCP the cycle normally gates on. */
bool wifi_manager_wait_ip6_routable(uint32_t timeout_ms);

/* Persist creds. `pass == ""` stores an empty password (open network);
 * `pass == NULL` keeps the currently-stored password unchanged. */
esp_err_t wifi_creds_save(const char *ssid, const char *pass);

/* Bring up STA with stored creds and block until connected or timeout.
 * Returns ESP_OK on success, ESP_ERR_TIMEOUT / ESP_FAIL otherwise. */
esp_err_t wifi_sta_connect_stored(void);

/* Stop the STA driver; safe to call before deep sleep. */
void wifi_sta_stop(void);
