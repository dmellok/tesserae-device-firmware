#include "rest_config.h"
#include "app_config.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

static const char *TAG = "rest_cfg";

/* NVS namespace + keys for the REST transport. */
#define NVS_NS_REST        "rest"
#define NVS_KEY_SERVER     "server"
#define NVS_KEY_TOKEN      "token"
#define NVS_KEY_PAIRING    "pairing"
#define NVS_KEY_DEVID      "devid"
#define NVS_KEY_ETAG       "etag"
#define NVS_KEY_SLEEP_S    "sleep_s"
#define NVS_KEY_BTN_WAKE   "btn_wake"
#define NVS_KEY_UI_STATE   "ui_state"
#if BOARD_HAS_TOUCH
#define NVS_KEY_TOUCH_EN   "touch_en"
#define NVS_KEY_TOUCH_LIN  "touch_lin"
#endif

static rest_config_t s_cfg;
static bool          s_loaded;
static char          s_devid[33];   /* cached "esp32_<mac>" default */

/* Trim a trailing '/' off the server origin so path concatenation is clean. */
static void strip_trailing_slash(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') s[--n] = '\0';
}

static void set_str(char *dst, size_t cap, const char *src)
{
    if (!src) return;                       /* NULL: leave unchanged */
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) dst[0] = '\0';
}

void rest_config_load(void)
{
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.sleep_s = SLEEP_INTERVAL_S;

    /* secrets.h dev defaults, if present (server_url only; token is per-device). */
#ifdef REST_DEFAULT_SERVER_URL
    set_str(s_cfg.server_url, sizeof s_cfg.server_url, REST_DEFAULT_SERVER_URL);
#endif
#ifdef REST_DEFAULT_PAIRING_CODE
    set_str(s_cfg.pairing_code, sizeof s_cfg.pairing_code, REST_DEFAULT_PAIRING_CODE);
#endif

    nvs_handle_t h;
    if (nvs_open(NVS_NS_REST, NVS_READONLY, &h) == ESP_OK) {
        load_str(h, NVS_KEY_SERVER,  s_cfg.server_url,     sizeof s_cfg.server_url);
        load_str(h, NVS_KEY_TOKEN,   s_cfg.device_token,   sizeof s_cfg.device_token);
        load_str(h, NVS_KEY_PAIRING, s_cfg.pairing_code,   sizeof s_cfg.pairing_code);
        load_str(h, NVS_KEY_DEVID,   s_cfg.device_id,      sizeof s_cfg.device_id);
        load_str(h, NVS_KEY_ETAG,    s_cfg.last_frame_etag,sizeof s_cfg.last_frame_etag);
        int32_t s = 0;
        if (nvs_get_i32(h, NVS_KEY_SLEEP_S, &s) == ESP_OK && s > 0) s_cfg.sleep_s = s;
        int32_t bw = 0;
        if (nvs_get_i32(h, NVS_KEY_BTN_WAKE, &bw) == ESP_OK && bw >= 0) s_cfg.button_wake_s = bw;
#if BOARD_HAS_TOUCH
        uint8_t te = 0;
        if (nvs_get_u8(h, NVS_KEY_TOUCH_EN, &te) == ESP_OK) s_cfg.touch_enabled = (te != 0);
        int32_t tl = 0;
        if (nvs_get_i32(h, NVS_KEY_TOUCH_LIN, &tl) == ESP_OK && tl >= 0) s_cfg.touch_linger_s = tl;
#endif
        nvs_close(h);
    }

#ifdef TESSERAE_TEST_SERVER_URL
    /* Bench-only override (never defined in release builds): point the cycle
     * at a test server and drop the token IN RAM ONLY, so the full discover ->
     * register -> frame -> status path runs against it without a settings
     * portal round-trip. NVS is not modified by the override itself; on a
     * normal build the device self-heals via MAC-matched discover. */
    set_str(s_cfg.server_url, sizeof s_cfg.server_url, TESSERAE_TEST_SERVER_URL);
    s_cfg.device_token[0] = '\0';
    ESP_LOGW(TAG, "TEST OVERRIDE: server='%s', token cleared (RAM)", s_cfg.server_url);
#endif

    strip_trailing_slash(s_cfg.server_url);
    s_loaded = true;
    ESP_LOGI(TAG, "loaded server='%s' id='%s' token=%s etag=%s sleep_s=%ld",
             s_cfg.server_url[0] ? s_cfg.server_url : "(none)",
             rest_config_device_id(),
             s_cfg.device_token[0] ? "set" : "(none)",
             s_cfg.last_frame_etag[0] ? "set" : "(none)",
             (long)s_cfg.sleep_s);
}

const rest_config_t *rest_config_get(void)
{
    if (!s_loaded) rest_config_load();
    return &s_cfg;
}

esp_err_t rest_config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_REST, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, NVS_KEY_SERVER, s_cfg.server_url);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_TOKEN,   s_cfg.device_token);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_PAIRING, s_cfg.pairing_code);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_DEVID,   s_cfg.device_id);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_ETAG,    s_cfg.last_frame_etag);
    if (err == ESP_OK) err = nvs_set_i32(h, NVS_KEY_SLEEP_S, s_cfg.sleep_s);
    if (err == ESP_OK) err = nvs_set_i32(h, NVS_KEY_BTN_WAKE, s_cfg.button_wake_s);
#if BOARD_HAS_TOUCH
    if (err == ESP_OK) err = nvs_set_u8(h, NVS_KEY_TOUCH_EN, s_cfg.touch_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(h, NVS_KEY_TOUCH_LIN, s_cfg.touch_linger_s);
#endif
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

const char *rest_config_device_id(void)
{
    if (s_cfg.device_id[0]) return s_cfg.device_id;
    if (s_devid[0] == '\0') {
        /* Default id: "<board model>_<suffix>", e.g. reTerminal_E1004_859878.
         * The suffix is the last 3 bytes of the MAC -- stable per device (the
         * id must not change across wakes) and unique enough to distinguish
         * units; the full MAC is still sent separately for the server match. */
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_devid, sizeof s_devid, "%s_%02x%02x%02x",
                 TESSERAE_DEVICE_MODEL, mac[3], mac[4], mac[5]);
    }
    return s_devid;
}

void rest_config_mac(char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool rest_config_has_server(void)
{
    return rest_config_get()->server_url[0] != '\0';
}

void rest_config_set_server(const char *url)
{
    set_str(s_cfg.server_url, sizeof s_cfg.server_url, url);
    strip_trailing_slash(s_cfg.server_url);
}
void rest_config_set_pairing(const char *code)     { set_str(s_cfg.pairing_code, sizeof s_cfg.pairing_code, code); }
void rest_config_set_device_id(const char *id)     { set_str(s_cfg.device_id, sizeof s_cfg.device_id, id); }
void rest_config_set_device_token(const char *tok) { set_str(s_cfg.device_token, sizeof s_cfg.device_token, tok); }
void rest_config_set_frame_etag(const char *etag)  { set_str(s_cfg.last_frame_etag, sizeof s_cfg.last_frame_etag, etag); }
void rest_config_set_sleep_s(int32_t s)            { if (s > 0) s_cfg.sleep_s = s; }

void rest_config_set_button_wake_s(int32_t s)
{
    if (s < 0)  s = 0;
    if (s > 60) s = 60;   /* server bounds it too; belt and braces */
    s_cfg.button_wake_s = s;
}

#if BOARD_HAS_TOUCH
void rest_config_set_touch(bool enabled, int32_t linger_s)
{
    if (linger_s < 0)  linger_s = 0;
    if (linger_s > 60) linger_s = 60;
    s_cfg.touch_enabled  = enabled;
    s_cfg.touch_linger_s = linger_s;
}
#endif

/* Persisted onboarding-splash state (a small standalone NVS u8, not part of the
 * main blob) so the panel is repainted only when the state actually changes --
 * not on every retry wake or USB dev-loop restart while still pending. */
uint8_t rest_config_get_ui_state(void)
{
    uint8_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_REST, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, NVS_KEY_UI_STATE, &v) != ESP_OK) v = 0;
        nvs_close(h);
    }
    return v;
}

void rest_config_set_ui_state(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_REST, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t cur = 0;
    /* Skip the write if unchanged -- avoids needless flash churn. */
    if (nvs_get_u8(h, NVS_KEY_UI_STATE, &cur) != ESP_OK || cur != v) {
        if (nvs_set_u8(h, NVS_KEY_UI_STATE, v) == ESP_OK) nvs_commit(h);
    }
    nvs_close(h);
}
