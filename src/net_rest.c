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
#include "button_report.h"
#include "ota_report.h"
#include "overlay.h"      /* OVERLAY_MAX_TARGETS (advertised capability cap) */
#include "touch3_run.h"   /* BOARD_TOUCH3 */
#include "wifi_manager.h"
#if BOARD_TOUCH3
#include "touch3.h"           /* T3_TOUCH_V, T3_MAX_PRIMS */
#include "panel/epd_panel.h"  /* epd_active_driver()->info.bpp */
#ifdef T3_HAVE_ICON_FONT
#include "phosphor_codepoints.h"   /* T3_PHOSPHOR_VERSION (the pin) */
#endif
#endif

#include "lwip/netdb.h"   /* getaddrinfo: IPv6-only server detection */
#include "sht4x.h"
#include "shtc3.h"

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

/* Sized for the largest JSON body we accept: a deck manifest (GET /deck) with
 * DECK_MAX_PAGES fully-linked pages runs to ~10 KB. Everything else is <4 KB. */
/* Shared response buffer, and the REAL ceiling on any JSON body: small_get()
 * copies out of here, so a caller's own buffer being larger buys nothing.
 * Raised from 16 KB for touch-v3 specs: 46 primitives run ~12 KB on their own,
 * and once the server ships the two glyph atlases their maps add ~3 KB each, so
 * a full spec lands around 18-25 KB and would have been silently TRUNCATED
 * (parse then fails, controls vanish, and the only clue is one WARN). */
#define REST_RX_MAX 32768

typedef struct { const char *name, *value; } rest_hdr_t;

/* ---- per-request capture (single request at a time) ---- */
static char     s_rx[REST_RX_MAX + 1];
static size_t   s_rx_len;
static bool     s_overflow;
static char     s_etag[80];
static int      s_retry_after;
static uint32_t s_server_date;

/* Pending front-button press to report with the next frame/status request.
 * Empty = none. Set by rest_set_button() on a button wake (see buttons.h). */
static button_report_t s_button;

void rest_set_button(const char *name, uint64_t event_id)
{
    button_report_set(&s_button, name, event_id);
}

bool rest_pending_button(char *name, size_t cap, uint64_t *event_id)
{
    if (!button_report_pending(&s_button)) return false;
    if (name && cap) snprintf(name, cap, "%s", s_button.name);
    if (event_id) *event_id = s_button.event_id;
    return true;
}

/* Deck cache capability + SD-paint report (sticky for this wake; see deck.h).
 * s_deck_capacity == 0 means no card mounted -> the capability object is
 * omitted entirely and a deck-unaware server sees exactly the old bodies. */
static uint64_t s_deck_capacity;
static char     s_deck_page[DECK_ID_CAP];
static char     s_deck_ver[DECK_VERSION_CAP];
static uint64_t s_frame_capacity;
static uint16_t s_frame_max;
static char     s_collection_id[FC_ID_CAP];
static char     s_collection_ver[FC_VERSION_CAP];
static char     s_collection_state[12];
static uint16_t s_collection_cached;
static char     s_collection_reason[24];
static uint32_t s_collection_total;

void rest_set_deck_capability(uint64_t capacity_bytes)
{
    s_deck_capacity = capacity_bytes;
}

void rest_set_frame_cache_capability(uint64_t capacity_bytes, uint16_t max_frames)
{
    s_frame_capacity = capacity_bytes;
    s_frame_max = max_frames;
}

void rest_set_collection_report(const char *id, const char *version,
                                uint16_t cached, uint32_t total,
                                const char *state)
{
    if (!id || !id[0] || !version || !version[0] || !state || !state[0]) {
        s_collection_id[0] = '\0';
        s_collection_ver[0] = '\0';
        s_collection_state[0] = '\0';
        s_collection_cached = 0;
        s_collection_total = 0;
        return;
    }
    snprintf(s_collection_id, sizeof s_collection_id, "%s", id);
    snprintf(s_collection_ver, sizeof s_collection_ver, "%s", version);
    snprintf(s_collection_state, sizeof s_collection_state, "%s", state);
    s_collection_cached = cached;
    s_collection_total = total;
}

void rest_set_collection_reason(const char *reason)
{
    snprintf(s_collection_reason, sizeof s_collection_reason, "%s",
             reason ? reason : "");
}

void rest_set_deck_painted(const char *page_id, const char *version)
{
    if (!page_id || !page_id[0] || !version || !version[0]) {
        s_deck_page[0] = '\0';
        s_deck_ver[0]  = '\0';
        return;
    }
    snprintf(s_deck_page, sizeof s_deck_page, "%s", page_id);
    snprintf(s_deck_ver,  sizeof s_deck_ver,  "%s", version);
}

/* Contract: capacity is capped at 64 MB so a huge card doesn't invite the
 * server to schedule an unbounded sync. */
#define DECK_CAPACITY_CAP (64ULL * 1024 * 1024)

static void add_deck_capability(cJSON *o)
{
    if (!s_deck_capacity) return;
    cJSON *dc = cJSON_AddObjectToObject(o, "deck_cache");
    if (!dc) return;
    cJSON_AddNumberToObject(dc, "schema", 1);
    uint64_t cap = s_deck_capacity < DECK_CAPACITY_CAP ? s_deck_capacity
                                                       : DECK_CAPACITY_CAP;
    cJSON_AddNumberToObject(dc, "capacity_bytes", (double)cap);
}

static void add_frame_cache_capability(cJSON *o)
{
    if (!s_frame_capacity || !s_frame_max) return;
    cJSON *fc = cJSON_AddObjectToObject(o, "frame_cache");
    if (!fc) return;
    cJSON_AddNumberToObject(fc, "schema", 1);
    uint64_t cap = s_frame_capacity < DECK_CAPACITY_CAP ? s_frame_capacity
                                                        : DECK_CAPACITY_CAP;
    cJSON_AddNumberToObject(fc, "capacity_bytes", (double)cap);
    cJSON_AddNumberToObject(fc, "max_frames", s_frame_max);
}

static void add_collection_report(cJSON *o)
{
    if (!s_collection_id[0]) return;
    cJSON *c = cJSON_AddObjectToObject(o, "collection");
    if (!c) return;
    cJSON_AddStringToObject(c, "id", s_collection_id);
    cJSON_AddStringToObject(c, "version", s_collection_ver);
    cJSON_AddNumberToObject(c, "cached", s_collection_cached);
    cJSON_AddNumberToObject(c, "total", (double)s_collection_total);
    cJSON_AddStringToObject(c, "state", s_collection_state);
    /* Why the last sync failed, when it did. The server validates `state`
     * against a fixed set and drops the whole report on anything unexpected, so
     * the reason cannot ride in there; it goes alongside as its own key. Older
     * servers ignore it, which is the point -- the device can start explaining
     * itself without waiting for a server release. */
    if (s_collection_reason[0])
        cJSON_AddStringToObject(c, "reason", s_collection_reason);
}

#if BOARD_HAS_TOUCH
/* Pending touch stroke to report with the next frame GET. s_touch_on gates it;
 * a NULL/empty digest disables (the server needs the displayed ETag). */
static bool     s_touch_on;
static int      s_touch_x0, s_touch_y0, s_touch_x1, s_touch_y1;
static uint32_t s_touch_ms;
static uint64_t s_touch_event;
static char     s_touch_digest[80];

void rest_set_touch(int x0, int y0, int x1, int y1, uint32_t ms,
                    const char *digest, uint64_t event_id)
{
    if (!digest || !digest[0]) { s_touch_on = false; s_touch_digest[0] = '\0'; return; }
    s_touch_on = true;
    s_touch_x0 = x0; s_touch_y0 = y0; s_touch_x1 = x1; s_touch_y1 = y1;
    s_touch_ms = ms; s_touch_event = event_id;
    /* Strip surrounding quotes off the ETag; the server tolerates either. */
    const char *d = digest;
    size_t n = strlen(d);
    if (n >= 2 && d[0] == '"' && d[n - 1] == '"') { d++; n -= 2; }
    if (n >= sizeof s_touch_digest) n = sizeof s_touch_digest - 1;
    memcpy(s_touch_digest, d, n);
    s_touch_digest[n] = '\0';
}
#endif /* BOARD_HAS_TOUCH */

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
        case 404:           return REST_NOT_FOUND;
        case 401:           return REST_UNAUTH;
        case 403:           return REST_FORBIDDEN;
        case 429:           return REST_RATELIMIT;
        default:            return REST_HTTP_ERR;
    }
}

/* IPv6 (issue #2): when the server is only reachable over IPv6 -- a bracketed
 * v6 literal in the URL, or a hostname with AAAA records but no A record --
 * wait briefly for SLAAC to produce a routable v6 address before the wake's
 * first request. The v4 "got ip" event the cycle gates on fires seconds before
 * SLAAC completes, so without this the connect races the address and loses.
 * Runs at most once per boot; v4-resolvable hosts detect that in one (lwIP-
 * cached) lookup and skip the wait entirely, so IPv4 behaviour is unchanged. */
#define REST_IP6_WAIT_MS 5000
static void wait_ip6_if_needed(const char *url)
{
    static bool s_done;
    if (s_done) return;
    s_done = true;

    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    bool need6 = false;
    if (host[0] == '[') {
        need6 = true;   /* bracketed IPv6 literal */
    } else {
        char name[128];
        size_t n = strcspn(host, ":/");
        if (n == 0 || n >= sizeof name) return;
        memcpy(name, host, n);
        name[n] = '\0';
        struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        if (getaddrinfo(name, NULL, &hints, &res) != 0 || res == NULL) return;
        bool has4 = false, has6 = false;
        for (struct addrinfo *a = res; a != NULL; a = a->ai_next) {
            if (a->ai_family == AF_INET)  has4 = true;
            if (a->ai_family == AF_INET6) has6 = true;
        }
        freeaddrinfo(res);
        need6 = has6 && !has4;
    }
    if (need6) {
        bool up = wifi_manager_wait_ip6_routable(REST_IP6_WAIT_MS);
        ESP_LOGI(TAG, "server is IPv6-only; routable v6 %s",
                 up ? "ready" : "NOT up in time (request may fail this wake)");
    }
}

/* Core request. On REST_OK/304/etc the accumulated body is at s_rx (NUL
 * terminated); *body_out points into it. Captured ETag/Retry-After are exposed
 * via the s_* statics and copied out below. */
static rest_status_t do_request(esp_http_client_method_t method, const char *url,
                                const rest_hdr_t *hdrs, int nh, const char *body,
                                const char **body_out, uint32_t timeout_ms)
{
    wait_ip6_if_needed(url);
    s_rx_len = 0; s_overflow = false; s_etag[0] = '\0';
    s_retry_after = 0; s_server_date = 0;
    if (body_out) *body_out = NULL;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = (int)timeout_ms,
        .event_handler = http_evt,
        .disable_auto_redirect = false,
    };
    /* Attach the CA bundle only for TLS. Setting crt_bundle_attach on a plain
     * http:// request is unnecessary and can leave the client mis-configured
     * (observed: esp_http_client_perform -> ESP_ERR_NOT_SUPPORTED on http). */
    if (strncmp(url, "https://", 8) == 0)
        cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return REST_NET_ERR;

    for (int i = 0; i < nh; i++)
        esp_http_client_set_header(cli, hdrs[i].name, hdrs[i].value);
    if (body) {
        esp_http_client_set_header(cli, "Content-Type", "application/json");
        esp_http_client_set_post_field(cli, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(cli);
    int http = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    /* esp_http_client_perform() returns an error for some statuses it tries to
     * auto-handle -- notably a 401 with no WWW-Authenticate header (which a
     * Bearer-token API never sends), yielding ESP_ERR_NOT_SUPPORTED. But the
     * status line + body were received, so if we got a status code we use it;
     * only a response-less failure (http <= 0) is a real transport error.
     * Without this, a 401 (e.g. a revoked token) surfaces as REST_NET_ERR and
     * the token is never wiped / re-onboarded. */
    if (http <= 0) {
        ESP_LOGW(TAG, "%s: transport error: %s", url, esp_err_to_name(err));
        return REST_NET_ERR;
    }

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

bool rest_probe_server_url(const char *server_url, uint32_t timeout_ms)
{
    if (!server_url ||
        (strncmp(server_url, "http://", 7) != 0 &&
         strncmp(server_url, "https://", 8) != 0)) return false;
    char url[224];
    int n = snprintf(url, sizeof url, "%s/api/app/v1", server_url);
    if (n <= 0 || (size_t)n >= sizeof url) return false;

    const char *body = NULL;
    if (do_request(HTTP_METHOD_GET, url, NULL, 0, NULL, &body, timeout_ms) != REST_OK ||
        !body) return false;
    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    const cJSON *product = cJSON_GetObjectItemCaseSensitive(root, "product");
    const cJSON *api = cJSON_GetObjectItemCaseSensitive(root, "api");
    const cJSON *name = cJSON_IsObject(api)
        ? cJSON_GetObjectItemCaseSensitive(api, "name") : NULL;
    bool ok = cJSON_IsString(product) && product->valuestring &&
              strcmp(product->valuestring, "tesserae") == 0 &&
              cJSON_IsString(name) && name->valuestring &&
              strcmp(name->valuestring, "companion") == 0;
    cJSON_Delete(root);
    return ok;
}

#if TESSERAE_OTA_CAPABILITY_ENABLED
static void add_ota_capability(cJSON *o)
{
    cJSON *ota = cJSON_AddObjectToObject(o, "ota");
    if (ota == NULL) return;
    cJSON_AddNumberToObject(ota, "schema", OTA_SCHEMA_VERSION);
    /* State report (contract "State reporting"): phase/reason/target_fw/
     * attempt_id of the latest attempt. Adds nothing while idle; the server
     * dedups repeated identical terminal reports. */
    ota_report_fill(ota);
}
#endif

#if BOARD_OVERLAY_PARTIAL
/* Local-overlay capability: this board has a partial-refresh panel driver.
 * Compile-time gated; boards without usable partial refresh never emit it. */
static void add_overlay_capability(cJSON *o)
{
    cJSON *ov = cJSON_AddObjectToObject(o, "overlay");
    if (!ov) return;
    cJSON_AddNumberToObject(ov, "schema", 2);   /* 2: post-action frame patches */
    /* Additive: lets the server trim target lists per device instead of
     * assuming a fixed cap (servers that ignore it keep sending <= 8). */
    cJSON_AddNumberToObject(ov, "max_targets", OVERLAY_MAX_TARGETS);
    /* proto2: extended atlas charset (digits+symbols+A-Z, <= 64 glyphs). */
    cJSON_AddNumberToObject(ov, "max_glyphs", 64);
}

/* Protocol v2 (device-owned touch): sticky server-side, advertised every
 * beat anyway to heal server restarts (proto2 contract, "capability"). */
static void add_proto_capability(cJSON *o)
{
    cJSON *p = cJSON_AddObjectToObject(o, "proto");
    if (p) cJSON_AddNumberToObject(p, "v", 2);
}

#if BOARD_TOUCH3
/* Touch v3 capability (contract §3 / firmware-spec §14). Advertised alongside
 * proto v2 -- the server picks the highest version it can serve, and v3 stays
 * dormant on a server that ignores it.
 *
 * "icons" is advertised only when a Phosphor weight is actually bundled
 * (T3_HAVE_ICON_FONT). Claiming it without the font would tell the server the
 * device can render icons by name when it cannot, and there is no atlas
 * fallback left to catch that -- every icon would silently vanish. The version
 * string is the PIN, taken straight from the generated codepoint map so the
 * advertised value and the glyphs actually bundled can never drift apart. */
static void add_touch3_capability(cJSON *o)
{
    cJSON *t = cJSON_AddObjectToObject(o, "touch");
    if (t) {
        cJSON_AddNumberToObject(t, "v", T3_TOUCH_V);
        cJSON *prims = cJSON_AddArrayToObject(t, "primitives");
        if (prims) {
            cJSON_AddItemToArray(prims, cJSON_CreateString("button"));
            cJSON_AddItemToArray(prims, cJSON_CreateString("switch"));
            cJSON_AddItemToArray(prims, cJSON_CreateString("slider"));
            cJSON_AddItemToArray(prims, cJSON_CreateString("stepper"));
        }
        cJSON_AddNumberToObject(t, "max_primitives", T3_MAX_PRIMS);
    }
    cJSON_AddBoolToObject(o, "partial_refresh", true);

    cJSON *p = cJSON_AddObjectToObject(o, "panel");
    if (p) {
        /* Both straight from the active driver's info, so a new panel family
         * declares its own capability once and cannot be missed here. */
        const epd_panel_info_t *pi = &epd_active_driver()->info;
        cJSON_AddNumberToObject(p, "depth_bpp", pi->bpp);
        cJSON_AddBoolToObject(p, "grayscale", pi->grayscale);
    }

#ifdef T3_HAVE_ICON_FONT
    cJSON *ic = cJSON_AddObjectToObject(o, "icons");
    if (ic) {
        cJSON_AddStringToObject(ic, "font", "phosphor");
        cJSON_AddStringToObject(ic, "version", T3_PHOSPHOR_VERSION);
        cJSON_AddStringToObject(ic, "weight", "bold");
    }
#endif
}
#endif /* BOARD_TOUCH3 */
#endif

/* Identity body shared by /discover and /register. Caller frees. */
/* can_stay_awake: may this device be offered always-on mode?
 *
 * Deliberately NOT part of the touch capability block it used to live in. It
 * used to be hardcoded true there, which meant two wrong things at once: only
 * the one board with a digitizer ever advertised it, and that board claimed the
 * capability whatever it was plugged into. Every board advertises it now (see
 * power_can_stay_awake for why the operator, not the board header, decides),
 * and it has to be re-sent on every heartbeat because a visible cell running
 * down retracts it mid-session.
 *
 * Always emitted explicitly, never omitted. The server treats an absent key as
 * "no new information" and keeps its last answer, so an explicit false is the
 * only way to retract the capability once granted. */
static void add_power_capability(cJSON *o)
{
    cJSON_AddBoolToObject(o, "can_stay_awake", power_can_stay_awake());
}

static char *identity_body(uint16_t panel_w, uint16_t panel_h,
                           const char *mac, const char *fw_version,
                           bool advertise_ota)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "device_id", rest_config_device_id());
    cJSON_AddStringToObject(o, "kind", TESSERAE_DEVICE_KIND);
    cJSON_AddNumberToObject(o, "panel_w", panel_w);
    cJSON_AddNumberToObject(o, "panel_h", panel_h);
    cJSON_AddStringToObject(o, "fw_version", fw_version);
    cJSON_AddStringToObject(o, "mac", mac ? mac : "");
#if TESSERAE_OTA_CAPABILITY_ENABLED
    if (advertise_ota) add_ota_capability(o);
#else
    (void)advertise_ota;
#endif
    /* Register advertises capabilities; discover stays identity-only. */
    if (advertise_ota) add_power_capability(o);
    if (advertise_ota) add_deck_capability(o);
    if (advertise_ota) add_frame_cache_capability(o);
#if BOARD_OVERLAY_PARTIAL
    if (advertise_ota) add_overlay_capability(o);
    if (advertise_ota) add_proto_capability(o);
#if BOARD_TOUCH3
    if (advertise_ota) add_touch3_capability(o);
#endif
#endif
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

    char *body = identity_body(panel_w, panel_h, mac, fw_version, false);
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
    out->button_wake_s = -1;

    char url[200];
    snprintf(url, sizeof url, "%s/api/v1/device/register", rest_config_get()->server_url);

    char *body = identity_body(panel_w, panel_h, mac, fw_version, true);
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
    if (cfg) {
        out->sleep_interval_s = json_get_int(cfg, "sleep_interval_s", -1);
        out->button_wake_s    = json_get_int(cfg, "button_wake_s", -1);
    }
    cJSON_Delete(r);
    return out->token[0] ? REST_OK : REST_HTTP_ERR;
}

rest_status_t rest_get_frame(rest_frame_out_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof *out);
    out->button_wake_s = -1;   /* only a 200 body can carry it */
    const rest_config_t *c = rest_config_get();
    char url[256];
    int un = snprintf(url, sizeof url, "%s/api/v1/device/%s/frame", c->server_url, rest_config_device_id());
    /* A wake action is dispatched on the GET so the server acts before it
     * responds. button_event_id dedups a retried request. A wake is button XOR
     * touch. */
    if (button_report_pending(&s_button) && un > 0 && un < (int)sizeof url) {
        un = button_report_append_frame_query(&s_button, url, sizeof url, un);
    }
#if BOARD_HAS_TOUCH
    else if (s_touch_on && un > 0 && un < (int)sizeof url) {
        /* Coordinates are in the served frame's pixel space; the server
         * classifies tap/swipe/slide and repaints in the same response. */
        snprintf(url + un, sizeof url - un,
                 "?touch_x0=%d&touch_y0=%d&touch_x1=%d&touch_y1=%d"
                 "&touch_ms=%u&touch_digest=%s&touch_event_id=%llu",
                 s_touch_x0, s_touch_y0, s_touch_x1, s_touch_y1,
                 (unsigned)s_touch_ms, s_touch_digest,
                 (unsigned long long)s_touch_event);
    }
#endif
    /* SD-paint report rides the same-wake frame GET too (contract). */
    if (s_deck_page[0]) {
        size_t cur = strlen(url);
        if (cur < sizeof url) {
            snprintf(url + cur, sizeof url - cur,
                     "%cdeck_page_id=%s&deck_version=%s",
                     strchr(url, '?') ? '&' : '?', s_deck_page, s_deck_ver);
        }
    }

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
    /* 200/204/304 prove the server received the frame request and therefore
     * dispatched its button action. Clear before the normal /status heartbeat
     * so one physical press produces one event. Network/auth/HTTP failures keep
     * it pending and /status remains the fallback delivery path. */
    button_report_finish_frame(
        &s_button,
        st == REST_OK || st == REST_NOT_MODIFIED || st == REST_NO_CONTENT
    );
    /* Capture the new ETag (from the response header) regardless of the JSON. */
    snprintf(out->etag, sizeof out->etag, "%s", s_etag);
    if (st != REST_OK) return st;   /* 304 / 204 / errors handled by caller */

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_HTTP_ERR;
    json_get_str(r, "url", out->url, sizeof out->url);
    json_get_str(r, "format", out->format, sizeof out->format);
    out->panel_w = (uint16_t)json_get_int(r, "panel_w", 0);
    out->panel_h = (uint16_t)json_get_int(r, "panel_h", 0);
    out->button_wake_s = json_get_int(r, "button_wake_s", -1);
    /* proto v2: the manifest block is the server's version signal. */
    cJSON *man = cJSON_GetObjectItemCaseSensitive(r, "manifest");
    if (cJSON_IsObject(man)) {
        json_get_str(man, "digest", out->manifest_digest,
                     sizeof out->manifest_digest);
        json_get_str(man, "url", out->manifest_url, sizeof out->manifest_url);
        out->has_manifest = out->manifest_digest[0] != '\0';
    }
    /* Touch v3: layout_digest, top-level or nested under "touch". Its absence
     * means this frame carries no v3 spec (touch3.h). */
    json_get_str(r, "layout_digest", out->layout_digest,
                 sizeof out->layout_digest);
    if (!out->layout_digest[0]) {
        cJSON *tv = cJSON_GetObjectItemCaseSensitive(r, "touch");
        if (cJSON_IsObject(tv))
            json_get_str(tv, "layout_digest", out->layout_digest,
                         sizeof out->layout_digest);
    }
    cJSON_Delete(r);
    return out->url[0] ? REST_OK : REST_HTTP_ERR;
}

rest_status_t rest_get_deck_manifest(char *buf, size_t cap, size_t *out_len,
                                     uint32_t timeout_ms)
{
    if (out_len) *out_len = 0;
    const rest_config_t *c = rest_config_get();
    char url[256];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/deck",
             c->server_url, rest_config_device_id());

    char auth[300];
    snprintf(auth, sizeof auth, "Bearer %s", c->device_token);
    rest_hdr_t hdrs[] = { { "Authorization", auth } };

    const char *rbody = NULL;
    rest_status_t st = do_request(HTTP_METHOD_GET, url, hdrs, 1, NULL, &rbody, timeout_ms);
    if (st != REST_OK) return st;   /* 204 = no deck bound (caller handles) */
    if (s_overflow || s_rx_len == 0 || s_rx_len >= cap) return REST_HTTP_ERR;
    memcpy(buf, rbody, s_rx_len);
    buf[s_rx_len] = '\0';
    if (out_len) *out_len = s_rx_len;
    return REST_OK;
}

rest_status_t rest_get_collection_manifest(char *buf, size_t cap,
                                           size_t *out_len,
                                           uint32_t timeout_ms)
{
    if (out_len) *out_len = 0;
    const rest_config_t *c = rest_config_get();
    char url[256];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/collection",
             c->server_url, rest_config_device_id());

    char auth[300];
    snprintf(auth, sizeof auth, "Bearer %s", c->device_token);
    rest_hdr_t hdrs[] = { { "Authorization", auth } };
    const char *rbody = NULL;
    rest_status_t st = do_request(HTTP_METHOD_GET, url, hdrs, 1, NULL,
                                  &rbody, timeout_ms);
    if (st != REST_OK) return st;
    if (s_overflow || s_rx_len == 0 || s_rx_len >= cap) return REST_HTTP_ERR;
    memcpy(buf, rbody, s_rx_len);
    buf[s_rx_len] = '\0';
    if (out_len) *out_len = s_rx_len;
    return REST_OK;
}

void rest_deck_frame_url(const char *digest, char *out, size_t cap)
{
    snprintf(out, cap, "%s/api/v1/device/%s/deck/frame/%s",
             rest_config_get()->server_url, rest_config_device_id(), digest);
}

const char *rest_bearer_token(void)
{
    return rest_config_get()->device_token;
}

/* Small authorised GET into a caller buffer (overlay spec / values doc). */
static rest_status_t small_get(const char *url, char *buf, size_t cap,
                               size_t *out_len, uint32_t timeout_ms)
{
    if (out_len) *out_len = 0;
    char auth[300];
    snprintf(auth, sizeof auth, "Bearer %s", rest_config_get()->device_token);
    rest_hdr_t hdrs[] = { { "Authorization", auth } };
    const char *rbody = NULL;
    rest_status_t st = do_request(HTTP_METHOD_GET, url, hdrs, 1, NULL, &rbody, timeout_ms);
    if (st != REST_OK) return st;
    if (s_overflow || s_rx_len == 0 || s_rx_len >= cap) return REST_HTTP_ERR;
    memcpy(buf, rbody, s_rx_len);
    buf[s_rx_len] = '\0';
    if (out_len) *out_len = s_rx_len;
    return REST_OK;
}

rest_status_t rest_get_overlay_spec(const char *digest, char *buf, size_t cap,
                                    size_t *out_len, uint32_t timeout_ms)
{
    char url[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/frame/overlay/%s",
             rest_config_get()->server_url, rest_config_device_id(), digest);
    return small_get(url, buf, cap, out_len, timeout_ms);
}

rest_status_t rest_get_frame_data(const char *digest, char *buf, size_t cap,
                                  size_t *out_len, uint32_t timeout_ms)
{
    char url[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/frame/data?digest=%s",
             rest_config_get()->server_url, rest_config_device_id(), digest);
    return small_get(url, buf, cap, out_len, timeout_ms);
}

rest_status_t rest_get_manifest(const char *digest, char *buf, size_t cap,
                                size_t *out_len, uint32_t timeout_ms)
{
    char url[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/frame/manifest?digest=%s",
             rest_config_get()->server_url, rest_config_device_id(), digest);
    return small_get(url, buf, cap, out_len, timeout_ms);
}

rest_status_t rest_get_frame_spec(const char *layout_digest, char *buf,
                                  size_t cap, size_t *out_len,
                                  uint32_t timeout_ms)
{
    char url[300];
    /* Omit the query string entirely when there is no digest to advertise --
     * a bare "?layout=" is not the same request as no param, and the param is
     * advisory anyway (see touch3_run.c). */
    if (layout_digest && layout_digest[0])
        snprintf(url, sizeof url, "%s/api/v1/device/%s/frame/spec?layout=%s",
                 rest_config_get()->server_url, rest_config_device_id(),
                 layout_digest);
    else
        snprintf(url, sizeof url, "%s/api/v1/device/%s/frame/spec",
                 rest_config_get()->server_url, rest_config_device_id());
    return small_get(url, buf, cap, out_len, timeout_ms);
}

rest_status_t rest_post_interact(const char *primitive_id,
                                 const char *interaction,
                                 bool has_value, float value,
                                 const char *layout_digest, uint64_t event_id,
                                 rest_interact_out_t *out,
                                 uint32_t timeout_ms)
{
    if (out) memset(out, 0, sizeof *out);
    char url[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/interact",
             rest_config_get()->server_url, rest_config_device_id());

    cJSON *o = cJSON_CreateObject();
    if (!o) return REST_NET_ERR;
    cJSON_AddStringToObject(o, "primitive_id", primitive_id);
    cJSON_AddStringToObject(o, "interaction", interaction);
    if (has_value) cJSON_AddNumberToObject(o, "value", (double)value);
    cJSON_AddStringToObject(o, "layout_digest", layout_digest);
    /* uint64 as a JSON number: ids stay <= 2^53 so the double round-trip is
     * exact (see button_report.h). */
    cJSON_AddNumberToObject(o, "event_id", (double)event_id);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return REST_NET_ERR;

    char auth[300];
    snprintf(auth, sizeof auth, "Bearer %s", rest_config_get()->device_token);
    rest_hdr_t hdrs[] = { { "Authorization", auth } };
    const char *rbody = NULL;
    rest_status_t st = do_request(HTTP_METHOD_POST, url, hdrs, 1, body, &rbody,
                                  timeout_ms);
    free(body);
    if (st != REST_OK || !out) return st;

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_OK;      /* no body / unparseable: keep optimistic */
    json_get_str(r, "outcome", out->outcome, sizeof out->outcome);
    /* Confirmed state may sit at the top level or under "confirmed"/"state".
     * Read liberally: the contract only promises "may carry confirmed state
     * for stateful primitives" (contract §6), and the server does not send any
     * yet -- absent means "unconfirmed", not "unchanged". */
    cJSON *scope = cJSON_GetObjectItemCaseSensitive(r, "confirmed");
    if (!cJSON_IsObject(scope)) scope = r;
    cJSON *stv = cJSON_GetObjectItemCaseSensitive(scope, "state");
    if (cJSON_IsString(stv)) {
        out->have_state = true;
        out->state = strcmp(stv->valuestring, "on") == 0;
    } else if (cJSON_IsBool(stv)) {
        out->have_state = true;
        out->state = cJSON_IsTrue(stv);
    }
    cJSON *vv = cJSON_GetObjectItemCaseSensitive(scope, "value");
    if (cJSON_IsNumber(vv)) {
        out->have_value = true;
        out->value = (float)vv->valuedouble;
    }
    cJSON_Delete(r);
    return REST_OK;
}

rest_status_t rest_get_bundle(char *buf, size_t cap, size_t *out_len,
                              uint32_t timeout_ms)
{
    char url[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/bundle",
             rest_config_get()->server_url, rest_config_device_id());
    return small_get(url, buf, cap, out_len, timeout_ms);
}

rest_status_t rest_post_tap(const char *region_id, const char *gesture,
                            int value, const char *digest, uint64_t event_id,
                            int x0, int y0, int x1, int y1,
                            char *outcome, size_t outcome_cap,
                            uint32_t timeout_ms)
{
    if (outcome && outcome_cap) outcome[0] = '\0';
    char url[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/tap",
             rest_config_get()->server_url, rest_config_device_id());

    cJSON *o = cJSON_CreateObject();
    if (!o) return REST_NET_ERR;
    cJSON_AddStringToObject(o, "region_id", region_id);
    cJSON_AddStringToObject(o, "gesture", gesture);
    if (value >= 0) cJSON_AddNumberToObject(o, "value", value);
    cJSON_AddStringToObject(o, "digest", digest);
    /* uint64 as a JSON number: the seeder keeps ids <= 2^53 so the double
     * round-trip is exact (see button_report.h). */
    cJSON_AddNumberToObject(o, "event_id", (double)event_id);
    cJSON_AddNumberToObject(o, "x0", x0);
    cJSON_AddNumberToObject(o, "y0", y0);
    cJSON_AddNumberToObject(o, "x1", x1);
    cJSON_AddNumberToObject(o, "y1", y1);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return REST_NET_ERR;

    char auth[300];
    snprintf(auth, sizeof auth, "Bearer %s", rest_config_get()->device_token);
    rest_hdr_t hdrs[] = { { "Authorization", auth } };
    const char *rbody = NULL;
    rest_status_t st = do_request(HTTP_METHOD_POST, url, hdrs, 1, body, &rbody,
                                  timeout_ms);
    free(body);
    if (st != REST_OK) return st;

    cJSON *r = cJSON_Parse(rbody);
    if (r) {
        if (outcome && outcome_cap)
            json_get_str(r, "outcome", outcome, outcome_cap);
        cJSON_Delete(r);
    }
    return REST_OK;
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
    out->button_wake_s = -1;
#if BOARD_HAS_TOUCH
    out->touch_enabled = -1;
    out->touch_linger_s = -1;
#endif
#ifdef BOARD_BUZZER_PIN
    out->beep_enabled = -1;
    out->beep_volume  = -1;
    out->beep_pattern[0] = '\0';
#endif
#if BOARD_OVERLAY_PARTIAL
    out->local_hh = out->local_mm = -1;
#endif

    const rest_config_t *c = rest_config_get();
    char url[256];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/status", c->server_url, rest_config_device_id());

    /* Sampled once into locals: on a fuel-gauge board these are I2C reads, and
     * battery_present() is answered by the same read rather than being a
     * compile-time constant, so it was previously called twice per heartbeat. */
    const bool have_battery = battery_present();
    const int  mv  = battery_read_mv();
    const int  pct = battery_read_pct();
    if (have_battery)
        ESP_LOGI(TAG, "status: battery=%d mV (%d%%), rssi=%d, ip=%s",
                 mv, pct, rssi, ip ? ip : "");
    else
        ESP_LOGI(TAG, "status: battery=none, rssi=%d, ip=%s", rssi, ip ? ip : "");
    cJSON *o = cJSON_CreateObject();
    /* OMIT rather than send 0 where the board cannot measure its cell. A zero
     * is a real reading everywhere else, so the server would render a flat
     * battery on every frame; an absent field is unambiguous. */
    if (have_battery) {
        cJSON_AddNumberToObject(o, "battery_mv", mv);
        cJSON_AddNumberToObject(o, "battery_pct", pct);
    }
    cJSON_AddNumberToObject(o, "rssi", rssi);
    cJSON_AddStringToObject(o, "ip", ip ? ip : "");
    /* next_sleep_s and sleep_until (below) both describe WHEN WE WILL WAKE, and
     * feed a server-side prediction that schedules renders to land just before
     * it. While always-on that prediction has nothing to predict, and feeding it
     * a number would make the server hold renders for a device that is sitting
     * there ready. Omit both; the server derives what it needs from
     * awake_poll_s. */
    if (!c->always_on)
        cJSON_AddNumberToObject(o, "next_sleep_s", next_sleep_s);
    cJSON_AddStringToObject(o, "fw_version", fw_version);
    cJSON_AddNumberToObject(o, "panel_w", panel_w);
    cJSON_AddNumberToObject(o, "panel_h", panel_h);
#if TESSERAE_OTA_CAPABILITY_ENABLED
    add_ota_capability(o);
#endif
#ifdef BOARD_HAS_SHT4X
    sht4x_sample_t environment;
    esp_err_t environment_err = sht4x_read(&environment);
    if (environment_err == ESP_OK) {
        ESP_LOGI(TAG, "status: environment=%.1f C, %.1f %%RH",
                 environment.temperature_c, environment.humidity_pct);
        cJSON_AddNumberToObject(o, "temperature_c", environment.temperature_c);
        cJSON_AddNumberToObject(o, "humidity_pct", environment.humidity_pct);
        cJSON_AddStringToObject(o, "env_sensor", "sht4x");
    } else {
        ESP_LOGW(TAG, "status: SHT4x read failed: %s", esp_err_to_name(environment_err));
    }
#elif defined(BOARD_HAS_SHTC3)
    shtc3_sample_t environment;
    esp_err_t environment_err = shtc3_read(&environment);
    if (environment_err == ESP_OK) {
        ESP_LOGI(TAG, "status: environment=%.1f C, %.1f %%RH",
                 environment.temperature_c, environment.humidity_pct);
        cJSON_AddNumberToObject(o, "temperature_c", environment.temperature_c);
        cJSON_AddNumberToObject(o, "humidity_pct", environment.humidity_pct);
        cJSON_AddStringToObject(o, "env_sensor", "shtc3");
    } else {
        ESP_LOGW(TAG, "status: SHTC3 read failed: %s", esp_err_to_name(environment_err));
    }
#endif
    if (sleep_until && !c->always_on)
        cJSON_AddNumberToObject(o, "sleep_until", (double)sleep_until);
    if (button_report_pending(&s_button)) { /* failed /frame fallback */
        cJSON_AddStringToObject(o, "button", s_button.name);
        cJSON_AddNumberToObject(o, "button_event_id", (double)s_button.event_id);
    }
    /* Every beat, on every board: the answer can change without a reboot when
     * a mains panel is unplugged or its cell runs down. */
    add_power_capability(o);
    add_deck_capability(o);
    add_frame_cache_capability(o);
#if BOARD_OVERLAY_PARTIAL
    add_overlay_capability(o);
    add_proto_capability(o);
#if BOARD_TOUCH3
    add_touch3_capability(o);   /* every beat: always_on can flip mid-session */
#endif
#endif
    /* proto2 status contract: tz rides every beat (server-config sourced;
     * empty until the server delivers one). */
    cJSON_AddStringToObject(o, "tz", c->tz);
    if (s_deck_page[0]) {
        /* The displayed frame came from the SD cache: these two fields are
         * the only signal the server needs to keep its nav state truthful
         * (locally-served events are deliberately NOT reported as actions). */
        cJSON_AddStringToObject(o, "deck_page_id", s_deck_page);
        cJSON_AddStringToObject(o, "deck_version", s_deck_ver);
    }
    add_collection_report(o);
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
    if (cfg) {
        out->sleep_interval_s = json_get_int(cfg, "sleep_interval_s", -1);
        out->button_wake_s    = json_get_int(cfg, "button_wake_s", -1);
#if BOARD_HAS_TOUCH
        cJSON *te = cJSON_GetObjectItemCaseSensitive(cfg, "touch_enabled");
        if (cJSON_IsBool(te))        out->touch_enabled = cJSON_IsTrue(te) ? 1 : 0;
        else if (cJSON_IsNumber(te)) out->touch_enabled = te->valueint ? 1 : 0;
        out->touch_linger_s = json_get_int(cfg, "touch_linger_s", -1);
#endif
#ifdef BOARD_BUZZER_PIN
        cJSON *be = cJSON_GetObjectItemCaseSensitive(cfg, "beep_enabled");
        if (cJSON_IsBool(be))        out->beep_enabled = cJSON_IsTrue(be) ? 1 : 0;
        else if (cJSON_IsNumber(be)) out->beep_enabled = be->valueint ? 1 : 0;
        out->beep_volume = json_get_int(cfg, "beep_volume", -1);
        json_get_str(cfg, "beep_pattern", out->beep_pattern, sizeof out->beep_pattern);
#endif
        char tz[40] = {0};
        json_get_str(cfg, "tz", tz, sizeof tz);
        if (tz[0] && strcmp(tz, c->tz) != 0) {
            rest_config_set_tz(tz);
            rest_config_save();
        }
        /* always_on is SERVER CONFIG, on the same channel as sleep_interval_s:
         * true = run without deep sleep (kiosk: GT911 stays hot and the SSE
         * state stream is held), false = the normal sleep cycle. Read every
         * poll and obey the latest value, so a mid-session change takes effect
         * on the next poll with no reboot. Accepts a bool or 0/1, matching
         * touch_enabled above. */
        cJSON *ao = cJSON_GetObjectItemCaseSensitive(cfg, "always_on");
        int ao_v = -1;
        if (cJSON_IsBool(ao))        ao_v = cJSON_IsTrue(ao) ? 1 : 0;
        else if (cJSON_IsNumber(ao)) ao_v = ao->valueint ? 1 : 0;
        if (ao_v >= 0 && (ao_v != 0) != c->always_on) {
            rest_config_set_always_on(ao_v != 0);
            rest_config_save();
        }
        /* awake_poll_s rides the same config block. It is the poll cadence
         * WHILE awake and has nothing to do with sleep_interval_s, which keeps
         * its meaning as the deep-sleep cadence and stays the value we fall
         * back to the moment always_on goes false.
         *
         * Persisted only on a real change: this arrives on every heartbeat and
         * writing NVS each time would wear flash for nothing. The setter drops
         * out-of-range values, so an unusable config leaves the last good
         * cadence in place rather than spinning the loop. */
        int32_t aw = json_get_int(cfg, "awake_poll_s", -1);
        if (aw > 0 && aw != c->awake_poll_s) {
            rest_config_set_awake_poll_s(aw);
            if (rest_config_get()->awake_poll_s == aw) {
                ESP_LOGI(TAG, "awake poll cadence -> %d s", (int)aw);
                rest_config_save();
            } else {
                ESP_LOGW(TAG, "ignoring awake_poll_s=%d (outside %d..%d)",
                         (int)aw, AWAKE_POLL_MIN_S, AWAKE_POLL_MAX_S);
            }
        }
    }
#if BOARD_OVERLAY_PARTIAL
    /* overlay_values / overlay_patches ride the status response; hand the
     * raw objects to the overlay engine (same semantics as the polled
     * /frame/data document; newest seq wins independently per stream). */
    cJSON *ov = cJSON_GetObjectItemCaseSensitive(r, "overlay_values");
    if (cJSON_IsObject(ov)) {
        char *raw = cJSON_PrintUnformatted(ov);
        if (raw) {
            snprintf(out->overlay_values, sizeof out->overlay_values, "%s", raw);
            free(raw);
        }
    }
    cJSON *op = cJSON_GetObjectItemCaseSensitive(r, "overlay_patches");
    if (cJSON_IsObject(op)) {
        char *raw = cJSON_PrintUnformatted(op);
        if (raw) {
            snprintf(out->overlay_patches, sizeof out->overlay_patches, "%s", raw);
            free(raw);
        }
    }
    /* proto v2 extras: the sync digest triple + clock discipline. */
    out->local_hh = json_get_int(r, "local_hh", -1);
    out->local_mm = json_get_int(r, "local_mm", -1);
    cJSON *sy = cJSON_GetObjectItemCaseSensitive(r, "sync");
    if (cJSON_IsObject(sy)) {
        char *raw = cJSON_PrintUnformatted(sy);
        if (raw) {
            snprintf(out->sync_obj, sizeof out->sync_obj, "%s", raw);
            free(raw);
        }
    }
#endif
    /* Deck resync signal: "deck": {"version": str}. Absent on servers that
     * don't speak decks yet; the sync tail then never runs. */
    cJSON *deck = cJSON_GetObjectItemCaseSensitive(r, "deck");
    if (cJSON_IsObject(deck)) {
        out->deck_present = true;
        json_get_str(deck, "version", out->deck_version, sizeof out->deck_version);
    }
    cJSON *collection = cJSON_GetObjectItemCaseSensitive(r, "collection");
    if (cJSON_IsObject(collection)) {
        out->collection_present = true;
        json_get_str(collection, "id", out->collection_id,
                     sizeof out->collection_id);
        json_get_str(collection, "kind", out->collection_kind,
                     sizeof out->collection_kind);
        json_get_str(collection, "version", out->collection_version,
                     sizeof out->collection_version);
        if (!out->collection_id[0] || !out->collection_kind[0] ||
            !out->collection_version[0]) out->collection_present = false;
    }
#if TESSERAE_OTA_CAPABILITY_ENABLED
    cJSON *ota = cJSON_GetObjectItemCaseSensitive(r, "ota");
    if (ota != NULL) {
        out->ota_present = true;
        const cJSON *payload = cJSON_IsObject(ota)
                                   ? cJSON_GetObjectItemCaseSensitive(ota, "payload")
                                   : NULL;
        const cJSON *signature = cJSON_IsObject(ota)
                                     ? cJSON_GetObjectItemCaseSensitive(ota, "signature")
                                     : NULL;
        if (!cJSON_IsString(payload) || payload->valuestring == NULL ||
            strlen(payload->valuestring) > OTA_MAX_PAYLOAD_B64URL ||
            !cJSON_IsString(signature) || signature->valuestring == NULL ||
            strlen(signature->valuestring) > OTA_ED25519_SIG_B64URL) {
            out->ota_reason = OTA_VERIFY_MALFORMED_DESCRIPTOR;
        } else {
            out->ota_reason = ota_descriptor_check(payload->valuestring,
                                                   signature->valuestring,
                                                   TESSERAE_DEVICE_KIND,
                                                   FW_VERSION,
                                                   &out->ota_manifest);
        }
    }
#endif
    cJSON_Delete(r);
    return REST_OK;
}
