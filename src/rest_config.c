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
#define NVS_KEY_DECK_ID    "deck_id"
#define NVS_KEY_DECK_PG    "deck_pg"
#define NVS_KEY_DECK_SV    "deck_sv"    /* synced manifest version */
#define NVS_KEY_DECK_RV    "deck_rv"    /* last server-announced version */
#define NVS_KEY_DECK_SD    "deck_sd"    /* displayed frame came from SD */
#define NVS_KEY_TZ         "tz"         /* IANA timezone (proto2) */
#define NVS_KEY_KIOSK      "kiosk"      /* always-on power policy */
/* Cloud relay (docs/relay/contract.md). Short keys: NVS caps them at 15 chars. */
#define NVS_KEY_RLY_URL    "rly_url"
#define NVS_KEY_RLY_CODE   "rly_code"
#define NVS_KEY_RLY_INST   "rly_inst"
#define NVS_KEY_RLY_DEV    "rly_dev"
#define NVS_KEY_RLY_TOK    "rly_tok"
#define NVS_KEY_RLY_ETAG   "rly_etag"
#define NVS_KEY_RLY_CETAG  "rly_cetag"   /* config doc, separate from frames */
#define NVS_KEY_RLY_KEY    "rly_key"     /* blob: derived frame key */
#define NVS_KEY_RLY_PRIV   "rly_priv"    /* blob: panel X25519 priv, pairing only */
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
        load_str(h, NVS_KEY_DECK_ID, s_cfg.deck_id,         sizeof s_cfg.deck_id);
        load_str(h, NVS_KEY_DECK_PG, s_cfg.deck_page,       sizeof s_cfg.deck_page);
        load_str(h, NVS_KEY_DECK_SV, s_cfg.deck_synced_ver, sizeof s_cfg.deck_synced_ver);
        load_str(h, NVS_KEY_DECK_RV, s_cfg.deck_srv_ver,    sizeof s_cfg.deck_srv_ver);
        uint8_t dsd = 0;
        if (nvs_get_u8(h, NVS_KEY_DECK_SD, &dsd) == ESP_OK) s_cfg.deck_sd_painted = (dsd != 0);
        load_str(h, NVS_KEY_TZ, s_cfg.tz, sizeof s_cfg.tz);
        uint8_t ko = 0;
        if (nvs_get_u8(h, NVS_KEY_KIOSK, &ko) == ESP_OK) s_cfg.always_on = (ko != 0);

        load_str(h, NVS_KEY_RLY_URL,  s_cfg.relay_url,     sizeof s_cfg.relay_url);
        load_str(h, NVS_KEY_RLY_CODE, s_cfg.relay_code,    sizeof s_cfg.relay_code);
        load_str(h, NVS_KEY_RLY_INST, s_cfg.relay_install, sizeof s_cfg.relay_install);
        load_str(h, NVS_KEY_RLY_DEV,  s_cfg.relay_device,  sizeof s_cfg.relay_device);
        load_str(h, NVS_KEY_RLY_TOK,  s_cfg.relay_token,   sizeof s_cfg.relay_token);
        load_str(h, NVS_KEY_RLY_ETAG, s_cfg.relay_etag,    sizeof s_cfg.relay_etag);
        load_str(h, NVS_KEY_RLY_CETAG, s_cfg.relay_config_etag,
                 sizeof s_cfg.relay_config_etag);
        size_t blen = sizeof s_cfg.relay_key;
        s_cfg.relay_have_key =
            nvs_get_blob(h, NVS_KEY_RLY_KEY, s_cfg.relay_key, &blen) == ESP_OK &&
            blen == sizeof s_cfg.relay_key;
        blen = sizeof s_cfg.relay_priv;
        s_cfg.relay_have_priv =
            nvs_get_blob(h, NVS_KEY_RLY_PRIV, s_cfg.relay_priv, &blen) == ESP_OK &&
            blen == sizeof s_cfg.relay_priv;
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
    /* Relay state is on this line because leaving it off actively misled a
     * debugging session: a relay-paired panel logged server='(none)'
     * token=(none) and looked completely unprovisioned, while it was in fact
     * fully configured on the other transport. Which transport a device is on
     * is the first thing worth knowing at boot. */
    ESP_LOGI(TAG, "loaded server='%s' id='%s' token=%s etag=%s sleep_s=%ld "
                  "relay=%s",
             s_cfg.server_url[0] ? s_cfg.server_url : "(none)",
             rest_config_device_id(),
             s_cfg.device_token[0] ? "set" : "(none)",
             s_cfg.last_frame_etag[0] ? "set" : "(none)",
             (long)s_cfg.sleep_s,
             !s_cfg.relay_url[0]      ? "(none)"
             : rest_config_relay_ready() ? "paired"
             : s_cfg.relay_code[0]    ? "pairing"
                                      : "url-only");
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
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_DECK_ID, s_cfg.deck_id);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_DECK_PG, s_cfg.deck_page);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_DECK_SV, s_cfg.deck_synced_ver);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_DECK_RV, s_cfg.deck_srv_ver);
    if (err == ESP_OK) err = nvs_set_u8(h, NVS_KEY_DECK_SD, s_cfg.deck_sd_painted ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_TZ, s_cfg.tz);
    if (err == ESP_OK) err = nvs_set_u8(h, NVS_KEY_KIOSK, s_cfg.always_on ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_RLY_URL,  s_cfg.relay_url);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_RLY_CODE, s_cfg.relay_code);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_RLY_INST, s_cfg.relay_install);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_RLY_DEV,  s_cfg.relay_device);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_RLY_TOK,  s_cfg.relay_token);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_RLY_ETAG, s_cfg.relay_etag);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_RLY_CETAG,
                                         s_cfg.relay_config_etag);
    /* Secrets are blobs, and are ERASED rather than written empty when absent
     * so a wiped key never lingers as stale bytes in flash. */
    if (err == ESP_OK) {
        if (s_cfg.relay_have_key)
            err = nvs_set_blob(h, NVS_KEY_RLY_KEY, s_cfg.relay_key,
                               sizeof s_cfg.relay_key);
        else if (nvs_erase_key(h, NVS_KEY_RLY_KEY) == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
    }
    if (err == ESP_OK) {
        if (s_cfg.relay_have_priv)
            err = nvs_set_blob(h, NVS_KEY_RLY_PRIV, s_cfg.relay_priv,
                               sizeof s_cfg.relay_priv);
        else if (nvs_erase_key(h, NVS_KEY_RLY_PRIV) == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
    }
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

void rest_config_set_deck_nav(const char *deck_id, const char *page_id)
{
    set_str(s_cfg.deck_id,   sizeof s_cfg.deck_id,   deck_id);
    set_str(s_cfg.deck_page, sizeof s_cfg.deck_page, page_id);
}
void rest_config_set_deck_synced_ver(const char *version)
{
    set_str(s_cfg.deck_synced_ver, sizeof s_cfg.deck_synced_ver, version);
}
void rest_config_set_deck_srv_ver(const char *version)
{
    set_str(s_cfg.deck_srv_ver, sizeof s_cfg.deck_srv_ver, version);
}
void rest_config_set_deck_sd_painted(bool painted)
{
    s_cfg.deck_sd_painted = painted;
}

void rest_config_set_tz(const char *tz)
{
    set_str(s_cfg.tz, sizeof s_cfg.tz, tz ? tz : "");
}

void rest_config_set_always_on(bool on)
{
    s_cfg.always_on = on;
}

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

/* ---- cloud relay (docs/relay/contract.md) -------------------------------- */

void rest_config_set_relay(const char *url, const char *code)
{
    if (url) {
        set_str(s_cfg.relay_url, sizeof s_cfg.relay_url, url);
        strip_trailing_slash(s_cfg.relay_url);
    }
    if (code) set_str(s_cfg.relay_code, sizeof s_cfg.relay_code, code);
}

void rest_config_set_relay_priv(const uint8_t priv[32])
{
    if (priv) {
        memcpy(s_cfg.relay_priv, priv, sizeof s_cfg.relay_priv);
        s_cfg.relay_have_priv = true;
    } else {
        memset(s_cfg.relay_priv, 0, sizeof s_cfg.relay_priv);
        s_cfg.relay_have_priv = false;
    }
}

void rest_config_set_relay_paired(const char *install_id, const char *device_id,
                                  const char *token, const uint8_t key[32])
{
    if (install_id) set_str(s_cfg.relay_install, sizeof s_cfg.relay_install, install_id);
    if (device_id)  set_str(s_cfg.relay_device,  sizeof s_cfg.relay_device,  device_id);
    if (token)      set_str(s_cfg.relay_token,   sizeof s_cfg.relay_token,   token);
    if (key) {
        memcpy(s_cfg.relay_key, key, sizeof s_cfg.relay_key);
        s_cfg.relay_have_key = true;
    }
    /* The code is single-use, and the private key has done its job: the frame
     * key is derived, so keeping the scalar only widens what a flash dump
     * yields. */
    s_cfg.relay_code[0] = '\0';
    rest_config_set_relay_priv(NULL);
}

void rest_config_set_relay_etag(const char *etag)
{
    set_str(s_cfg.relay_etag, sizeof s_cfg.relay_etag, etag ? etag : "");
}

void rest_config_set_relay_config_etag(const char *etag)
{
    set_str(s_cfg.relay_config_etag, sizeof s_cfg.relay_config_etag,
            etag ? etag : "");
}

void rest_config_clear_relay(void)
{
    s_cfg.relay_code[0] = '\0';
    s_cfg.relay_install[0] = '\0';
    s_cfg.relay_device[0] = '\0';
    s_cfg.relay_token[0] = '\0';
    s_cfg.relay_etag[0] = '\0';
    /* The config doc is sealed with the OLD frame key, so its etag is
     * meaningless against a new pairing: keeping it would make the first
     * post-repair sync 304 and silently skip adopting the real config. */
    s_cfg.relay_config_etag[0] = '\0';
    memset(s_cfg.relay_key, 0, sizeof s_cfg.relay_key);
    s_cfg.relay_have_key = false;
    rest_config_set_relay_priv(NULL);
    /* relay_url survives: it is operator configuration, not pairing state, so
     * a re-pair only needs a fresh code. */
}

bool rest_config_relay_ready(void)
{
    if (!s_loaded) rest_config_load();
    return s_cfg.relay_url[0] && s_cfg.relay_install[0] && s_cfg.relay_device[0] &&
           s_cfg.relay_token[0] && s_cfg.relay_have_key;
}
