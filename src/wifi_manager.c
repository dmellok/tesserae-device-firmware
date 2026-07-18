#include "wifi_manager.h"
#include "app_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_events;
#define BIT_CONNECTED  BIT0
#define BIT_FAIL       BIT1

static int s_retries = 0;
static int s_max_retries = WIFI_CONNECT_RETRIES;   /* lowered for the fast attempt */
static esp_netif_t *s_sta_netif = NULL;
static bool s_inited = false;

/* Only auto-connect on STA_START during an intentional wifi_sta_connect_stored()
 * call. Provisioning brings the STA up just to scan (see provisioning.c
 * do_wifi_scan); auto-connecting there would put the STA in a "connecting"
 * state that blocks the scan. */
static bool s_autoconnect = false;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_autoconnect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_autoconnect) {
            /* not our connect attempt (e.g. a scan tore the STA down) */
        } else if (s_retries < s_max_retries) {
            s_retries++;
            ESP_LOGW(TAG, "disconnect; retry %d/%d", s_retries, s_max_retries);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, BIT_FAIL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_inited) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, NULL, NULL));

    s_inited = true;
    return ESP_OK;
}

bool wifi_creds_present(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) == ESP_OK) {
        size_t len = 0;
        esp_err_t err = nvs_get_str(h, NVS_KEY_SSID, NULL, &len);
        nvs_close(h);
        if (err == ESP_OK && len > 1) return true;
    }
    /* Fallback: secrets.h compile-time default counts as "have creds" too,
     * so a dev board boots straight into STA without round-tripping the
     * captive portal. */
    return WIFI_DEFAULT_SSID[0] != '\0';
}

bool wifi_creds_get_ssid(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return false;
    out[0] = '\0';

    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) == ESP_OK) {
        size_t len = out_sz;
        esp_err_t err = nvs_get_str(h, NVS_KEY_SSID, out, &len);
        nvs_close(h);
        if (err == ESP_OK && out[0]) return true;
    }
    if (WIFI_DEFAULT_SSID[0] != '\0') {
        strncpy(out, WIFI_DEFAULT_SSID, out_sz - 1);
        out[out_sz - 1] = '\0';
        return true;
    }
    return false;
}

bool wifi_manager_get_sta_ip(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return false;
    out[0] = '\0';

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) return false;
    snprintf(out, out_sz, IPSTR, IP2STR(&ip.ip));
    return true;
}

esp_err_t wifi_creds_save(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        if (pass) {
            err = nvs_set_str(h, NVS_KEY_PASS, pass);
        } else {
            /* pass == NULL: keep the stored password (the always-on portal
             * sends a blank field to mean "leave unchanged"). If none is
             * stored yet, write empty so the key always exists. */
            size_t l = 0;
            if (nvs_get_str(h, NVS_KEY_PASS, NULL, &l) == ESP_ERR_NVS_NOT_FOUND) {
                err = nvs_set_str(h, NVS_KEY_PASS, "");
            }
        }
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t load_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) == ESP_OK) {
        size_t l = ssid_sz;
        esp_err_t err = nvs_get_str(h, NVS_KEY_SSID, ssid, &l);
        if (err == ESP_OK) {
            l = pass_sz;
            err = nvs_get_str(h, NVS_KEY_PASS, pass, &l);
            if (err == ESP_ERR_NVS_NOT_FOUND) { pass[0] = '\0'; err = ESP_OK; }
            nvs_close(h);
            return err;
        }
        nvs_close(h);
    }
    /* NVS unset -- try the compile-time default from secrets.h. */
    if (WIFI_DEFAULT_SSID[0] != '\0') {
        strncpy(ssid, WIFI_DEFAULT_SSID, ssid_sz - 1);
        ssid[ssid_sz - 1] = '\0';
        strncpy(pass, WIFI_DEFAULT_PASS, pass_sz - 1);
        pass[pass_sz - 1] = '\0';
        return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

/* Fast-connect hint: the BSSID + channel of the AP we last associated with.
 * Reusing them lets esp_wifi skip the ~1-2 s all-channel scan on a wake. */
static void save_ap_hint(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h) != ESP_OK) return;
    /* Skip the write if unchanged, to spare flash wear across wakes. */
    uint8_t cur_bssid[6]; size_t l = sizeof cur_bssid; uint8_t cur_chan = 0;
    bool same = (nvs_get_blob(h, NVS_KEY_BSSID, cur_bssid, &l) == ESP_OK && l == 6 &&
                 memcmp(cur_bssid, ap.bssid, 6) == 0 &&
                 nvs_get_u8(h, NVS_KEY_CHAN, &cur_chan) == ESP_OK && cur_chan == ap.primary);
    if (!same) {
        nvs_set_blob(h, NVS_KEY_BSSID, ap.bssid, 6);
        nvs_set_u8(h, NVS_KEY_CHAN, ap.primary);
        nvs_commit(h);
    }
    nvs_close(h);
}

static bool load_ap_hint(uint8_t bssid[6], uint8_t *chan)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = 6;
    bool ok = (nvs_get_blob(h, NVS_KEY_BSSID, bssid, &l) == ESP_OK && l == 6 &&
               nvs_get_u8(h, NVS_KEY_CHAN, chan) == ESP_OK && *chan >= 1 && *chan <= 14);
    nvs_close(h);
    return ok;
}

static void clear_ap_hint(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_KEY_BSSID);
    nvs_erase_key(h, NVS_KEY_CHAN);
    nvs_commit(h);
    nvs_close(h);
}

/* One STA connect attempt. If bssid != NULL, targets that BSSID/channel directly
 * (fast, no scan). max_retries bounds the disconnect-retry loop. */
static esp_err_t connect_once(const char *ssid, const char *pass,
                              const uint8_t *bssid, uint8_t chan, int max_retries)
{
    s_events = xEventGroupCreate();
    s_retries = 0;
    s_max_retries = max_retries;
    s_autoconnect = true;   /* enable STA_START -> connect for this attempt */

    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid,     ssid, sizeof(wc.sta.ssid)     - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (bssid) {
        memcpy(wc.sta.bssid, bssid, 6);
        wc.sta.bssid_set = true;
        wc.sta.channel   = chan;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to '%s'%s", ssid, bssid ? " (fast)" : "");
    EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_CONNECTED | BIT_FAIL, pdTRUE, pdFALSE,
        pdMS_TO_TICKS((uint32_t)WIFI_CONNECT_TIMEOUT_MS * (max_retries + 1)));

    if (bits & BIT_CONNECTED) return ESP_OK;
    if (bits & BIT_FAIL)      return ESP_FAIL;
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_sta_connect_stored(void)
{
    char ssid[33] = {0};
    char pass[65] = {0};
    esp_err_t err = load_creds(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no stored creds: %s", esp_err_to_name(err));
        return err;
    }

    /* Fast path: target the last AP's BSSID/channel to skip the scan. A stale
     * hint (AP moved channel / roamed) fails fast, then we clear it and fall back
     * to a normal full-scan connect. */
    uint8_t bssid[6], chan;
    if (load_ap_hint(bssid, &chan)) {
        err = connect_once(ssid, pass, bssid, chan, WIFI_FAST_CONNECT_RETRIES);
        if (err == ESP_OK) { save_ap_hint(); return ESP_OK; }
        ESP_LOGW(TAG, "fast connect failed; clearing hint + full scan");
        clear_ap_hint();
        wifi_sta_stop();   /* tear down before the fallback attempt */
    }

    err = connect_once(ssid, pass, NULL, 0, WIFI_CONNECT_RETRIES);
    if (err == ESP_OK) save_ap_hint();
    return err;
}

void wifi_sta_stop(void)
{
    s_autoconnect = false;   /* stop the disconnect handler from retrying */
    esp_wifi_disconnect();
    esp_wifi_stop();
}
