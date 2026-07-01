/*
 * net_rest.c: Tesserae REST API client on esp_http_client. See net_rest.h.
 *
 * One request at a time. An event handler accumulates the (small JSON) response
 * body into a static buffer and captures the ETag / Retry-After / Date response
 * headers. The server's Date header sets the wall clock (settimeofday), so the
 * REST path needs no SNTP round-trip on a LAN. Ported from the pico-bin client;
 * the endpoint shapes, JSON fields, and status mapping match it exactly.
 */
#include "net_rest.h"
#include "rest_config.h"
#include "app_config.h"
#include "battery.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "rest";

#define REST_RX_MAX 4096

typedef struct { const char *name, *value; } rest_hdr_t;

/* ---- per-request capture (single request at a time) ---- */
static char     s_rx[REST_RX_MAX + 1];
static size_t   s_rx_len;
static bool     s_overflow;
static char     s_etag[80];
static int      s_retry_after;
static uint32_t s_server_date;

/* Days since the Unix epoch for a civil date (Howard Hinnant's algorithm). */
static long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097L + (long)doe - 719468L;
}

/* Parse an RFC 1123 HTTP Date ("Sun, 06 Nov 1994 08:49:37 GMT") to a Unix
 * epoch. Returns 0 if it does not look like a plausible recent timestamp. */
static uint32_t parse_http_date(const char *v)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *comma = strchr(v, ',');
    const char *p = comma ? comma + 1 : v;
    while (*p == ' ') p++;

    int d = 0, y = 0, hh = 0, mm = 0, ss = 0; char mon[4] = {0};
    if (sscanf(p, "%d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return 0;
    const char *mp = strstr(months, mon);
    if (!mp || y < 2020 || y > 2100) return 0;
    unsigned m = (unsigned)((mp - months) / 3) + 1u;

    long long e = (long long)days_from_civil(y, m, (unsigned)d) * 86400LL
                  + hh * 3600 + mm * 60 + ss;
    return (e > 1500000000LL) ? (uint32_t)e : 0;   /* sanity: past ~2017 */
}

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(evt->header_key, "ETag") == 0) {
            const char *v = evt->header_value; size_t len = strlen(v);
            if (len >= 2 && v[0] == '"' && v[len - 1] == '"') { v++; len -= 2; }
            if (len >= sizeof s_etag) len = sizeof s_etag - 1;
            memcpy(s_etag, v, len); s_etag[len] = '\0';
        } else if (strcasecmp(evt->header_key, "Retry-After") == 0) {
            s_retry_after = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "Date") == 0) {
            s_server_date = parse_http_date(evt->header_value);
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (s_rx_len + evt->data_len <= REST_RX_MAX) {
            memcpy(s_rx + s_rx_len, evt->data, evt->data_len);
            s_rx_len += evt->data_len;
        } else {
            s_overflow = true;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static rest_status_t map_status(int http)
{
    switch (http) {
        case 200: case 201: return REST_OK;
        case 204:           return REST_NO_CONTENT;
        case 304:           return REST_NOT_MODIFIED;
        case 401:           return REST_UNAUTH;
        case 403:           return REST_FORBIDDEN;
        case 429:           return REST_RATELIMIT;
        default:            return REST_HTTP_ERR;
    }
}

/* Core request. On REST_OK/304/etc the accumulated body is at s_rx (NUL
 * terminated); *body_out points into it. Captured ETag/Retry-After are exposed
 * via the s_* statics and copied out below. */
static rest_status_t do_request(esp_http_client_method_t method, const char *url,
                                const rest_hdr_t *hdrs, int nh, const char *body,
                                const char **body_out, uint32_t timeout_ms)
{
    s_rx_len = 0; s_overflow = false; s_etag[0] = '\0';
    s_retry_after = 0; s_server_date = 0;
    if (body_out) *body_out = NULL;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = (int)timeout_ms,
        .event_handler = http_evt,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* for https servers */
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return REST_NET_ERR;

    for (int i = 0; i < nh; i++)
        esp_http_client_set_header(cli, hdrs[i].name, hdrs[i].value);
    if (body) {
        esp_http_client_set_header(cli, "Content-Type", "application/json");
        esp_http_client_set_post_field(cli, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(cli);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: transport error: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return REST_NET_ERR;
    }
    int http = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (s_overflow) ESP_LOGW(TAG, "response truncated at %d bytes", REST_RX_MAX);
    s_rx[s_rx_len] = '\0';
    if (body_out) *body_out = s_rx;

    /* Server Date header is the authoritative LAN clock (no SNTP on this path). */
    if (s_server_date) {
        struct timeval tv = { .tv_sec = (time_t)s_server_date, .tv_usec = 0 };
        settimeofday(&tv, NULL);
    }

    ESP_LOGI(TAG, "<- %d (%u bytes)", http, (unsigned)s_rx_len);
    return map_status(http);
}

/* ---- small JSON helpers ---- */

static void json_get_str(const cJSON *o, const char *k, char *out, size_t cap)
{
    out[0] = '\0';
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) snprintf(out, cap, "%s", v->valuestring);
}

static int32_t json_get_int(const cJSON *o, const char *k, int32_t dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int32_t)v->valuedouble : dflt;
}

/* Identity body shared by /discover and /register. Caller frees. */
static char *identity_body(uint16_t panel_w, uint16_t panel_h,
                           const char *mac, const char *fw_version)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "device_id", rest_config_device_id());
    cJSON_AddStringToObject(o, "kind", TESSERAE_DEVICE_KIND);
    cJSON_AddNumberToObject(o, "panel_w", panel_w);
    cJSON_AddNumberToObject(o, "panel_h", panel_h);
    cJSON_AddStringToObject(o, "fw_version", fw_version);
    cJSON_AddStringToObject(o, "mac", mac ? mac : "");
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return body;
}

rest_status_t rest_discover(uint16_t panel_w, uint16_t panel_h,
                            const char *mac, const char *fw_version,
                            rest_discover_out_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof *out);
    out->sleep_interval_s = -1;
    out->retry_after_s = 30;

    char url[200];
    snprintf(url, sizeof url, "%s/api/v1/device/discover", rest_config_get()->server_url);

    char *body = identity_body(panel_w, panel_h, mac, fw_version);
    if (!body) return REST_NET_ERR;

    const char *rbody = NULL;
    rest_status_t st = do_request(HTTP_METHOD_POST, url, NULL, 0, body, &rbody, timeout_ms);
    free(body);
    if (st == REST_RATELIMIT && s_retry_after > 0) out->retry_after_s = s_retry_after;
    if (st != REST_OK) return st;

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_HTTP_ERR;
    out->registered  = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "registered"));
    out->server_time = (uint32_t)json_get_int(r, "server_time", 0);
    int32_t ra = json_get_int(r, "retry_after_s", 30);
    out->retry_after_s = (ra > 0) ? (int)ra : 30;
    if (out->registered) {
        json_get_str(r, "device_token", out->token, sizeof out->token);
        json_get_str(r, "device_id", out->device_id, sizeof out->device_id);
        cJSON *cfg = cJSON_GetObjectItemCaseSensitive(r, "config");
        if (cfg) out->sleep_interval_s = json_get_int(cfg, "sleep_interval_s", -1);
    }
    cJSON_Delete(r);
    if (out->registered && out->token[0] == '\0') return REST_HTTP_ERR;
    return REST_OK;
}

rest_status_t rest_register(uint16_t panel_w, uint16_t panel_h,
                            const char *mac, const char *fw_version,
                            rest_register_out_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof *out);
    out->sleep_interval_s = -1;

    char url[200];
    snprintf(url, sizeof url, "%s/api/v1/device/register", rest_config_get()->server_url);

    char *body = identity_body(panel_w, panel_h, mac, fw_version);
    if (!body) return REST_NET_ERR;

    rest_hdr_t hdrs[] = { { "X-Pairing-Code", rest_config_get()->pairing_code } };
    const char *rbody = NULL;
    rest_status_t st = do_request(HTTP_METHOD_POST, url, hdrs, 1, body, &rbody, timeout_ms);
    free(body);
    if (st == REST_RATELIMIT && s_retry_after > 0) out->retry_after_s = s_retry_after;
    if (st != REST_OK) return st;

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_HTTP_ERR;
    json_get_str(r, "device_token", out->token, sizeof out->token);
    json_get_str(r, "device_id", out->device_id, sizeof out->device_id);
    out->server_time = (uint32_t)json_get_int(r, "server_time", 0);
    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(r, "config");
    if (cfg) out->sleep_interval_s = json_get_int(cfg, "sleep_interval_s", -1);
    cJSON_Delete(r);
    return out->token[0] ? REST_OK : REST_HTTP_ERR;
}

rest_status_t rest_get_frame(rest_frame_out_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof *out);
    const rest_config_t *c = rest_config_get();
    char url[256];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/frame", c->server_url, rest_config_device_id());

    char auth[300], inm[128];
    snprintf(auth, sizeof auth, "Bearer %s", c->device_token);
    rest_hdr_t hdrs[2] = { { "Authorization", auth } };
    int nh = 1;
    if (c->last_frame_etag[0]) {
        snprintf(inm, sizeof inm, "\"%s\"", c->last_frame_etag);
        hdrs[1] = (rest_hdr_t){ "If-None-Match", inm };
        nh = 2;
    }

    const char *rbody = NULL;
    rest_status_t st = do_request(HTTP_METHOD_GET, url, hdrs, nh, NULL, &rbody, timeout_ms);
    /* Capture the new ETag (from the response header) regardless of the JSON. */
    snprintf(out->etag, sizeof out->etag, "%s", s_etag);
    if (st != REST_OK) return st;   /* 304 / 204 / errors handled by caller */

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_HTTP_ERR;
    json_get_str(r, "url", out->url, sizeof out->url);
    json_get_str(r, "format", out->format, sizeof out->format);
    out->panel_w = (uint16_t)json_get_int(r, "panel_w", 0);
    out->panel_h = (uint16_t)json_get_int(r, "panel_h", 0);
    cJSON_Delete(r);
    return out->url[0] ? REST_OK : REST_HTTP_ERR;
}

rest_status_t rest_post_status(int rssi, const char *ip,
                               uint16_t panel_w, uint16_t panel_h,
                               int32_t next_sleep_s, uint32_t sleep_until,
                               const char *fw_version,
                               rest_status_out_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof *out);
    out->next_poll_s = -1;
    out->sleep_interval_s = -1;

    const rest_config_t *c = rest_config_get();
    char url[256];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/status", c->server_url, rest_config_device_id());

    int mv = battery_read_mv();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "battery_mv", mv);
    cJSON_AddNumberToObject(o, "battery_pct", battery_pct(mv));
    cJSON_AddNumberToObject(o, "rssi", rssi);
    cJSON_AddStringToObject(o, "ip", ip ? ip : "");
    cJSON_AddNumberToObject(o, "next_sleep_s", next_sleep_s);
    cJSON_AddStringToObject(o, "fw_version", fw_version);
    cJSON_AddNumberToObject(o, "panel_w", panel_w);
    cJSON_AddNumberToObject(o, "panel_h", panel_h);
    if (sleep_until) cJSON_AddNumberToObject(o, "sleep_until", (double)sleep_until);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return REST_NET_ERR;

    char auth[300];
    snprintf(auth, sizeof auth, "Bearer %s", c->device_token);
    rest_hdr_t hdrs[] = { { "Authorization", auth } };

    const char *rbody = NULL;
    rest_status_t st = do_request(HTTP_METHOD_POST, url, hdrs, 1, body, &rbody, timeout_ms);
    free(body);
    if (st == REST_RATELIMIT && s_retry_after > 0) out->retry_after_s = s_retry_after;
    if (st != REST_OK) return st;

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_OK;   /* 200 with an unparseable body: nothing to merge */
    out->next_poll_s = json_get_int(r, "next_poll_s", -1);
    out->server_time = (uint32_t)json_get_int(r, "server_time", 0);
    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(r, "config");
    if (cfg) out->sleep_interval_s = json_get_int(cfg, "sleep_interval_s", -1);
    cJSON_Delete(r);
    return REST_OK;
}
