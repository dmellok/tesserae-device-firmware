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
static esp_netif_t *s_sta_netif = NULL;
static bool s_inited = false;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries < WIFI_CONNECT_RETRIES) {
            s_retries++;
            ESP_LOGW(TAG, "disconnect; retry %d/%d", s_retries, WIFI_CONNECT_RETRIES);
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

esp_err_t wifi_sta_connect_stored(void)
{
    char ssid[33] = {0};
    char pass[65] = {0};
    esp_err_t err = load_creds(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no stored creds: %s", esp_err_to_name(err));
        return err;
    }

    s_events = xEventGroupCreate();
    s_retries = 0;

    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid,     ssid, sizeof(wc.sta.ssid)     - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to '%s'", ssid);
    EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_CONNECTED | BIT_FAIL, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS * WIFI_CONNECT_RETRIES));

    if (bits & BIT_CONNECTED) return ESP_OK;
    if (bits & BIT_FAIL)      return ESP_FAIL;
    return ESP_ERR_TIMEOUT;
}

void wifi_sta_stop(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
}
