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

#define WIFI_SCAN_MAX_NETWORKS 24

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_network_t;

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

/* Current associated AP signal in dBm, or 0 when STA is not associated. */
int wifi_manager_get_rssi(void);

/* Block until the STA holds a ROUTABLE (global or unique-local) IPv6 address,
 * or the timeout passes. Returns immediately once one exists. Used by the REST
 * layer before the first request of a wake when the server host is only
 * reachable over IPv6 (issue #2) -- SLAAC needs a couple of seconds after
 * association, longer than the v4 DHCP the cycle normally gates on. */
bool wifi_manager_wait_ip6_routable(uint32_t timeout_ms);

/* Persist creds. `pass == ""` stores an empty password (open network);
 * `pass == NULL` keeps the currently-stored password unchanged. */
esp_err_t wifi_creds_save(const char *ssid, const char *pass);

/* Erase the stored SSID, password, and fast-connect hint. Compile-time
 * development defaults are intentionally unaffected. */
esp_err_t wifi_creds_clear(void);

/* Bounded active scan used by BLE setup and the captive portal. Duplicate
 * SSIDs collapse to the strongest AP. */
esp_err_t wifi_scan_networks(wifi_network_t *out, size_t cap, size_t *count);

/* True while the Bluetooth controller is enabled, i.e. Wi-Fi is sharing the
 * radio. The driver rejects a custom active-scan dwell in that state -- it logs
 * "Should use default active scan time parameter for WiFi scan when Bluetooth
 * is enabled" and the scan can come back short. Scan callers use this to fall
 * back to the driver's default dwell; always false on boards built without
 * Bluetooth. */
bool wifi_bt_coex_active(void);

/* Test credentials without persisting them. The caller owns the decision to
 * commit only after its server check also succeeds. */
esp_err_t wifi_sta_connect_credentials(const char *ssid, const char *pass);

/* Bring up STA with stored creds and block until connected or timeout.
 * Returns ESP_OK on success, ESP_ERR_TIMEOUT / ESP_FAIL otherwise. */
esp_err_t wifi_sta_connect_stored(void);

/* Stop the STA driver; safe to call before deep sleep. */
void wifi_sta_stop(void);
