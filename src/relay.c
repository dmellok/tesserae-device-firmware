/* relay.c -- Tesserae cloud-relay transport. See relay.h. */

#include "relay.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "battery.h"
#include "epd_driver.h"     /* EPD_WIDTH / EPD_HEIGHT via app_config */
#include "image_fetcher.h"
#include "relay_crypto.h"
#include "relay_wire.h"
#include "rest_config.h"

static const char *TAG = "relay";

/* ETag of a frame that decrypted but has not reached the glass yet. Held here
 * until relay_commit_frame(), so a frame that fails to paint is re-fetched next
 * wake instead of being skipped by a 304 forever. */
static char s_pending_etag[80];

#define RELAY_JSON_MAX   2048    /* pairing + status replies are small */
#define RELAY_HTTP_MS    10000

/* ---- small JSON HTTP helper --------------------------------------------
 * Pairing and status are tiny JSON round trips, so they use their own
 * accumulate-into-a-static-buffer client rather than net_rest's (which is
 * bound to the home server's URL and auth). Frames go through
 * image_fetch_conditional() instead: those are megabytes and need streaming. */

typedef struct { char *buf; size_t cap; size_t len; bool overflow; } rx_t;

static esp_err_t on_evt(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    rx_t *rx = e->user_data;
    if (!rx || !rx->buf) return ESP_OK;
    if (rx->len + e->data_len >= rx->cap) { rx->overflow = true; return ESP_OK; }
    memcpy(rx->buf + rx->len, e->data, e->data_len);
    rx->len += e->data_len;
    rx->buf[rx->len] = '\0';
    return ESP_OK;
}

/* One JSON request. body == NULL means GET. Returns the HTTP status, or a
 * negative value on a transport failure. */
static int relay_json(const char *url, const char *method, const char *body,
                      const char *bearer, char *out, size_t out_cap)
{
    if (out && out_cap) out[0] = '\0';
    rx_t rx = { .buf = out, .cap = out_cap, .len = 0, .overflow = false };

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_evt,
        .user_data = &rx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = RELAY_HTTP_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return -1;

    esp_http_client_set_method(cli, strcmp(method, "POST") == 0
                                    ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (bearer && bearer[0]) {
        char auth[300];
        snprintf(auth, sizeof auth, "Bearer %s", bearer);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    if (body) {
        esp_http_client_set_header(cli, "Content-Type", "application/json");
        esp_http_client_set_post_field(cli, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK && status <= 0) {
        ESP_LOGW(TAG, "%s %s: %s", method, url, esp_err_to_name(err));
        return -1;
    }
    if (rx.overflow) ESP_LOGW(TAG, "response > %u bytes, truncated",
                              (unsigned)out_cap);
    return status;
}

/* ---- state --------------------------------------------------------------- */

bool relay_ready(void)
{
    return rest_config_relay_ready();
}

bool relay_pairing_pending(void)
{
    const rest_config_t *c = rest_config_get();
    return c->relay_url[0] && c->relay_code[0] && !rest_config_relay_ready();
}

/* ---- pairing ------------------------------------------------------------- */

/* Submit our public key for the operator's code. 404 means the code is unknown
 * or expired -- a terminal answer, not something to retry into a lockout. */
static relay_pair_result_t pair_submit(const rest_config_t *c)
{
    uint8_t priv[RELAY_PRIV_LEN], pub[RELAY_PUB_LEN];
    relay_keypair(priv, pub);

    char pub_b64[RELAY_B64_KEY_CAP];
    if (!relay_b64url_encode(pub_b64, sizeof pub_b64, pub, sizeof pub))
        return RELAY_PAIR_ERROR;

    /* Self-report the hardware facts the panel already knows, so the operator
     * does not have to retype them when adding a remote panel. All optional;
     * a board that declares neither macro simply omits them (see relay_wire.h).
     * The values are per-board and come from the Tesserae hardware catalog --
     * notably the mono panels are esp32_bw_client, NOT the esp32_client default,
     * and being wrong here silently mis-packs every frame. */
#ifdef TESSERAE_RELAY_MODEL
    const char *model = TESSERAE_RELAY_MODEL;
#else
    const char *model = NULL;
#endif
#ifdef TESSERAE_RELAY_GAMUT
    const char *gamut = TESSERAE_RELAY_GAMUT;
#else
    const char *gamut = NULL;
#endif
    char body[512];
    if (!relay_build_pair_body(body, sizeof body, c->relay_code, pub_b64,
                               EPD_WIDTH, EPD_HEIGHT, model, gamut)) {
        ESP_LOGE(TAG, "could not build the pair request body");
        return RELAY_PAIR_ERROR;
    }

    char url[256];
    snprintf(url, sizeof url, "%s/v1/pair", c->relay_url);
    char resp[RELAY_JSON_MAX];
    int st = relay_json(url, "POST", body, NULL, resp, sizeof resp);

    if (st == 404) {
        ESP_LOGW(TAG, "pairing code rejected (expired or unknown); clearing");
        rest_config_clear_relay();
        rest_config_save();
        return RELAY_PAIR_EXPIRED;
    }
    if (st < 200 || st >= 300) {
        ESP_LOGW(TAG, "POST /v1/pair -> %d", st);
        return RELAY_PAIR_ERROR;
    }

    /* Persist the private key BEFORE reporting progress: pairing polls can span
     * deep sleeps, and losing the scalar would strand the code (which is
     * single-use, so it could not simply be retried). */
    rest_config_set_relay_priv(priv);
    rest_config_save();
    ESP_LOGI(TAG, "pairing submitted (%dx%d, model=%s, gamut=%s); waiting for "
                  "the home instance", EPD_WIDTH, EPD_HEIGHT,
             model ? model : "(none)", gamut ? gamut : "(none)");
    return RELAY_PAIR_WAITING;
}

/* Poll for completion and, on "ready", derive + persist the frame key.
 *
 * pairStatus has NO expiry check (confirmed against the deployed Worker), so
 * this may poll across as many deep-sleep wakes as home's completion poller
 * needs. The 10-minute code expiry is enforced only by POST /v1/pair, which is
 * why pair_submit() runs exactly once -- see relay_pair_step(). */
static relay_pair_result_t pair_poll(const rest_config_t *c)
{
    char url[256];
    snprintf(url, sizeof url, "%s/v1/pair/%s", c->relay_url, c->relay_code);
    char resp[RELAY_JSON_MAX];
    int st = relay_json(url, "GET", NULL, NULL, resp, sizeof resp);

    if (st == 404) {
        ESP_LOGW(TAG, "pairing code expired before completion; clearing");
        rest_config_clear_relay();
        rest_config_save();
        return RELAY_PAIR_EXPIRED;
    }
    if (st < 200 || st >= 300) return RELAY_PAIR_ERROR;

    relay_pairing_t pr;
    switch (relay_parse_ready(resp, strlen(resp), &pr)) {
    case RELAY_READY_PENDING:
        return RELAY_PAIR_WAITING;          /* home has not completed it yet */
    case RELAY_READY_MALFORMED:
        ESP_LOGE(TAG, "pairing response malformed or missing a field");
        return RELAY_PAIR_ERROR;
    default:
        break;
    }

    if (!c->relay_have_priv) {
        /* Our scalar is gone (NVS wiped mid-pair). The code is single-use for
         * the POST, so there is no way back: start over. */
        ESP_LOGE(TAG, "no panel private key held; re-pair required");
        rest_config_clear_relay();
        rest_config_save();
        return RELAY_PAIR_EXPIRED;
    }

    uint8_t key[RELAY_KEY_LEN];
    if (!relay_derive_key(key, c->relay_priv, pr.home_pub)) {
        ESP_LOGE(TAG, "frame key derivation failed (degenerate peer key)");
        return RELAY_PAIR_ERROR;
    }

    rest_config_set_relay_paired(pr.install_id, pr.device_id, pr.device_token,
                                 key);
    rest_config_set_relay_etag("");         /* new mailbox: nothing to match */
    rest_config_save();
    memset(key, 0, sizeof key);
    ESP_LOGI(TAG, "paired: install=%s device=%s", pr.install_id, pr.device_id);
    return RELAY_PAIR_DONE;
}

relay_pair_result_t relay_pair_step(void)
{
    const rest_config_t *c = rest_config_get();
    if (!c->relay_url[0] || !c->relay_code[0]) return RELAY_PAIR_IDLE;
    if (rest_config_relay_ready()) return RELAY_PAIR_IDLE;
    return c->relay_have_priv ? pair_poll(c) : pair_submit(c);
}

/* ---- frames -------------------------------------------------------------- */

relay_frame_result_t relay_fetch_frame(uint8_t **frame, size_t *len,
                                       uint8_t **owned)
{
    if (frame) *frame = NULL;
    if (len) *len = 0;
    if (owned) *owned = NULL;
    if (!rest_config_relay_ready()) return RELAY_FRAME_ERROR;

    const rest_config_t *c = rest_config_get();
    char url[320];
    if (!relay_mailbox_url(url, sizeof url, c->relay_url, c->relay_install,
                           c->relay_device, "frame")) {
        ESP_LOGE(TAG, "cannot build the frame URL from stored ids");
        return RELAY_FRAME_ERROR;
    }

    fetched_image_t img;
    char etag[80];
    int status = 0;
    esp_err_t err = image_fetch_conditional(url, c->relay_token,
                                            c->relay_etag, etag, sizeof etag,
                                            &status, &img);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "frame fetch failed (http %d): %s", status,
                 esp_err_to_name(err));
        return RELAY_FRAME_ERROR;
    }
    if (status == 304) return RELAY_FRAME_UNCHANGED;
    if (status == 204) return RELAY_FRAME_NONE;
    if (!img.data || img.len == 0) return RELAY_FRAME_ERROR;

    uint8_t *plain = NULL;
    size_t plain_len = 0;
    if (!relay_unseal(img.data, img.len, c->relay_key, &plain, &plain_len)) {
        /* A failed GCM tag is the one error that must never be painted: the
         * mailbox does not match our key, so the bytes are someone else's (or
         * corrupt). Drop the cached ETag so the next poll refetches rather
         * than 304-ing onto the same bad blob forever. */
        ESP_LOGE(TAG, "sealed frame failed authentication (%u bytes); dropping",
                 (unsigned)img.len);
        free(img.data);
        rest_config_set_relay_etag("");
        rest_config_save();
        return RELAY_FRAME_ERROR;
    }

    /* Stash the ETag; only committed once the frame actually paints. */
    s_pending_etag[0] = '\0';
    snprintf(s_pending_etag, sizeof s_pending_etag, "%s", etag);

    if (frame) *frame = plain;
    if (len) *len = plain_len;
    if (owned) *owned = img.data;    /* plain points INTO this allocation */

    ESP_LOGI(TAG, "frame %u bytes sealed -> %u plaintext (etag %s)",
             (unsigned)img.len, (unsigned)plain_len,
             etag[0] ? etag : "(none)");
    return RELAY_FRAME_NEW;
}

void relay_commit_frame(void)
{
    if (!s_pending_etag[0]) return;
    rest_config_set_relay_etag(s_pending_etag);
    rest_config_save();
    s_pending_etag[0] = '\0';
}

/* ---- status -------------------------------------------------------------- */

bool relay_post_status(int rssi, const char *ip, uint16_t panel_w,
                       uint16_t panel_h, const char *fw_version)
{
    if (!rest_config_relay_ready()) return false;
    const rest_config_t *c = rest_config_get();

    cJSON *o = cJSON_CreateObject();
    if (!o) return false;
    /* Same shape a REST client posts to a home /status endpoint, so the home
     * instance can run it through its normal heartbeat pipeline unchanged. */
    cJSON_AddStringToObject(o, "device_id", c->relay_device);
    cJSON_AddStringToObject(o, "fw_version", fw_version ? fw_version : "");
    cJSON_AddNumberToObject(o, "panel_w", panel_w);
    cJSON_AddNumberToObject(o, "panel_h", panel_h);
    if (ip && ip[0]) cJSON_AddStringToObject(o, "ip", ip);
    if (rssi != 0) cJSON_AddNumberToObject(o, "rssi", rssi);
    int mv = battery_read_mv();
    if (mv > 0) {
        cJSON_AddNumberToObject(o, "battery_mv", mv);
        cJSON_AddNumberToObject(o, "battery_pct", battery_pct(mv));
    }
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return false;

    char url[320];
    if (!relay_mailbox_url(url, sizeof url, c->relay_url, c->relay_install,
                           c->relay_device, "status")) {
        free(body);
        return false;
    }
    int st = relay_json(url, "POST", body, c->relay_token, NULL, 0);
    free(body);

    if (st < 200 || st >= 300) {
        ESP_LOGW(TAG, "status post -> %d", st);
        return false;
    }
    return true;
}
