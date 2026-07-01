#include "mqtt_handler.h"
#include "app_config.h"
#include "mqtt_config.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"
#include "nvs.h"

static const char *TAG = "mqtt";

#define BIT_GOT_MSG  BIT0
#define BIT_FAILED   BIT1
#define BIT_PUB_DONE BIT2

typedef struct {
    EventGroupHandle_t events;
    mqtt_job_t *out;
    const char *update_topic;
    const char *config_topic;
    const char *status_topic;
} ctx_t;

typedef struct {
    EventGroupHandle_t events;
    const char *status_topic;
    const char *payload;
    int payload_len;
    int msg_id;
} pub_ctx_t;

/* Per-device topics, derived from mqtt_config_t.device_id once per session.
 * Static so the pointers handed to esp-mqtt outlive the call. */
static char s_update_topic[96];
static char s_config_topic[96];
static char s_status_topic[96];

static void build_topics(const char *device_id)
{
    snprintf(s_update_topic, sizeof s_update_topic, "tesserae/%s/frame/bin", device_id);
    snprintf(s_config_topic, sizeof s_config_topic, "tesserae/%s/config",    device_id);
    snprintf(s_status_topic, sizeof s_status_topic, "tesserae/%s/status",    device_id);
}

/* ---------- payload parsers ---------- */

/* Cheap-and-good JSON URL extractor. Looks for "url" : "<value>" with
 * tolerance for whitespace. Falls back to treating the whole payload as
 * a bare URL if it starts with "http". */
static bool extract_url(const char *data, size_t len, char *dst, size_t dst_sz)
{
    if (len == 0) return false;

    /* Bare URL case */
    if (len < dst_sz - 1 && (
            strncmp(data, "http://",  7) == 0 ||
            strncmp(data, "https://", 8) == 0)) {
        memcpy(dst, data, len);
        dst[len] = '\0';
        while (len && (dst[len-1] == ' ' || dst[len-1] == '\r' ||
                       dst[len-1] == '\n' || dst[len-1] == '\t')) {
            dst[--len] = '\0';
        }
        return dst[0] != '\0';
    }

    /* JSON case: find "url" */
    const char *p = memmem(data, len, "\"url\"", 5);
    if (!p) return false;
    p += 5;
    while (p < data + len && (*p == ' ' || *p == '\t')) p++;
    if (p >= data + len || *p != ':') return false;
    p++;
    while (p < data + len && (*p == ' ' || *p == '\t')) p++;
    if (p >= data + len || *p != '"') return false;
    p++;
    const char *end = memchr(p, '"', (data + len) - p);
    if (!end) return false;

    size_t vlen = end - p;
    if (vlen >= dst_sz) vlen = dst_sz - 1;
    memcpy(dst, p, vlen);
    dst[vlen] = '\0';
    return vlen > 0;
}

/* Pull a top-level integer field "<key>": <int> out of a JSON payload.
 * Tolerates whitespace and optional negative sign. */
static bool extract_int(const char *data, size_t len, const char *key, int32_t *out)
{
    char needle[48];
    int nlen = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nlen <= 0 || nlen >= (int)sizeof(needle)) return false;

    const char *p = memmem(data, len, needle, nlen);
    if (!p) return false;
    p += nlen;

    while (p < data + len && (*p == ' ' || *p == '\t')) p++;
    if (p >= data + len || *p != ':') return false;
    p++;
    while (p < data + len && (*p == ' ' || *p == '\t')) p++;
    if (p >= data + len) return false;

    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    if (p >= data + len || !isdigit((unsigned char)*p)) return false;

    int32_t v = 0;
    while (p < data + len && isdigit((unsigned char)*p)) {
        v = v * 10 + (*p - '0');
        p++;
    }
    *out = v * sign;
    return true;
}

/* ---------- config-topic side effects ---------- */

static void apply_config_payload(const char *data, size_t len)
{
    int32_t interval = 0;
    if (!extract_int(data, len, "sleep_interval_s", &interval)) {
        ESP_LOGW(TAG, "config payload had no sleep_interval_s; ignoring");
        return;
    }
    if (interval < SLEEP_INTERVAL_MIN_S || interval > SLEEP_INTERVAL_MAX_S) {
        ESP_LOGW(TAG, "sleep_interval_s=%ld out of bounds [%d, %d]; ignoring",
                 (long)interval, SLEEP_INTERVAL_MIN_S, SLEEP_INTERVAL_MAX_S);
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_KEY_SLEEP_S, interval);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved sleep_interval_s=%ld to NVS", (long)interval);
}

/* ---------- event handling ---------- */

static bool topic_eq(const char *needle, const char *data, int len)
{
    return (int)strlen(needle) == len && strncmp(needle, data, len) == 0;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ctx_t *ctx = arg;
    esp_mqtt_event_handle_t e = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected; subscribing to %s + %s",
                 ctx->update_topic, ctx->config_topic);
        esp_mqtt_client_subscribe(e->client, ctx->update_topic, 1);
        esp_mqtt_client_subscribe(e->client, ctx->config_topic, 1);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "data on %.*s (%d bytes)",
                 e->topic_len, e->topic, e->data_len);
        if (topic_eq(ctx->config_topic, e->topic, e->topic_len)) {
            apply_config_payload(e->data, e->data_len);
        } else if (topic_eq(ctx->update_topic, e->topic, e->topic_len)) {
            if (extract_url(e->data, e->data_len,
                            ctx->out->url, sizeof(ctx->out->url))) {
                ESP_LOGI(TAG, "url: %s", ctx->out->url);
                xEventGroupSetBits(ctx->events, BIT_GOT_MSG);
            } else {
                ESP_LOGW(TAG, "payload had no usable url");
            }
        } else {
            ESP_LOGW(TAG, "unexpected topic; ignoring");
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "mqtt error");
        xEventGroupSetBits(ctx->events, BIT_FAILED);
        break;

    default:
        break;
    }
}

/* ---------- public API ---------- */

esp_err_t mqtt_fetch_retained(mqtt_job_t *job)
{
    if (!job) return ESP_ERR_INVALID_ARG;
    memset(job, 0, sizeof(*job));

    mqtt_config_t cfg_nvs;
    mqtt_config_load(&cfg_nvs);
    build_topics(cfg_nvs.device_id);

    ctx_t ctx = {
        .events = xEventGroupCreate(),
        .out = job,
        .update_topic = s_update_topic,
        .config_topic = s_config_topic,
        .status_topic = s_status_topic,
    };

    /* LWT: a NON-retained will. The broker delivers it to any live subscriber
     * on ungraceful disconnect (keepalive timeout during deep sleep, TCP drop,
     * power loss) so Tesserae learns the device went dark -- but because it is
     * NOT retained it does not overwrite the retained heartbeat. That retained
     * {...,"kind","panel_w","panel_h"} message is what survives across sleep
     * cycles and feeds Tesserae's discovery/register flow; a retained will here
     * would clobber it with a kind-less tombstone for the whole sleep window.
     * (esp_mqtt_client_stop() is graceful and doesn't trigger the will at all.) */
    static const char k_lwt_payload[] = "{\"state\":\"offline\"}";

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = cfg_nvs.uri,
        .credentials.client_id = MQTT_CLIENT_ID,
        .session.keepalive = 30,
        .session.last_will = {
            .topic   = s_status_topic,
            .msg     = k_lwt_payload,
            .msg_len = sizeof(k_lwt_payload) - 1,
            .qos     = 1,
            .retain  = 0,
        },
    };
    if (cfg_nvs.user[0]) {
        cfg.credentials.username = cfg_nvs.user;
        cfg.credentials.authentication.password = cfg_nvs.pass;
    }

    esp_mqtt_client_handle_t cli = esp_mqtt_client_init(&cfg);
    if (!cli) { vEventGroupDelete(ctx.events); return ESP_FAIL; }

    esp_mqtt_client_register_event(cli, ESP_EVENT_ANY_ID, on_event, &ctx);
    esp_err_t err = esp_mqtt_client_start(cli);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(cli);
        vEventGroupDelete(ctx.events);
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(
        ctx.events, BIT_GOT_MSG | BIT_FAILED,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(MQTT_WAIT_MS));

    esp_mqtt_client_stop(cli);
    esp_mqtt_client_destroy(cli);
    vEventGroupDelete(ctx.events);

    if (bits & BIT_GOT_MSG) return ESP_OK;
    if (bits & BIT_FAILED)  return ESP_FAIL;
    return ESP_ERR_TIMEOUT;
}

/* ---------- one-shot heartbeat publish ---------- */

/* Standalone PUBACK-confirming event handler used only by mqtt_publish_status.
 * Publishes on CONNECTED, signals BIT_PUB_DONE when the broker acks the
 * matching msg_id, BIT_FAILED on transport error. */
static void on_pub_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    pub_ctx_t *ctx = arg;
    esp_mqtt_event_handle_t e = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ctx->msg_id = esp_mqtt_client_publish(
            e->client, ctx->status_topic,
            ctx->payload, ctx->payload_len,
            /* qos */ 1, /* retain */ 1);
        if (ctx->msg_id < 0) {
            ESP_LOGE(TAG, "publish enqueue failed");
            xEventGroupSetBits(ctx->events, BIT_FAILED);
        } else {
            ESP_LOGI(TAG, "publishing heartbeat to %s (%d bytes, msg_id=%d)",
                     ctx->status_topic, ctx->payload_len, ctx->msg_id);
        }
        break;

    case MQTT_EVENT_PUBLISHED:
        if (e->msg_id == ctx->msg_id) {
            xEventGroupSetBits(ctx->events, BIT_PUB_DONE);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "mqtt error during publish");
        xEventGroupSetBits(ctx->events, BIT_FAILED);
        break;

    default:
        break;
    }
}

esp_err_t mqtt_publish_status(const char *payload)
{
    if (!payload || !*payload) return ESP_ERR_INVALID_ARG;

    mqtt_config_t cfg_nvs;
    mqtt_config_load(&cfg_nvs);
    build_topics(cfg_nvs.device_id);

    pub_ctx_t ctx = {
        .events       = xEventGroupCreate(),
        .status_topic = s_status_topic,
        .payload      = payload,
        .payload_len  = (int)strlen(payload),
        .msg_id       = -1,
    };

    /* Same LWT contract as the fetch path: NON-retained will so the broker
     * delivers offline to live subscribers without ever overwriting the
     * retained heartbeat. Short keepalive -- this is a few-second session. */
    static const char k_lwt_payload[] = "{\"state\":\"offline\"}";

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = cfg_nvs.uri,
        .credentials.client_id = MQTT_CLIENT_ID,
        .session.keepalive = 10,
        .session.last_will = {
            .topic   = s_status_topic,
            .msg     = k_lwt_payload,
            .msg_len = sizeof(k_lwt_payload) - 1,
            .qos     = 1,
            .retain  = 0,
        },
    };
    if (cfg_nvs.user[0]) {
        cfg.credentials.username = cfg_nvs.user;
        cfg.credentials.authentication.password = cfg_nvs.pass;
    }

    esp_mqtt_client_handle_t cli = esp_mqtt_client_init(&cfg);
    if (!cli) { vEventGroupDelete(ctx.events); return ESP_FAIL; }

    esp_mqtt_client_register_event(cli, ESP_EVENT_ANY_ID, on_pub_event, &ctx);
    esp_err_t err = esp_mqtt_client_start(cli);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(cli);
        vEventGroupDelete(ctx.events);
        return err;
    }

    /* Connect + publish + PUBACK normally finishes in <500 ms; 5 s gives
     * generous room for slow brokers without bloating render-wake length. */
    EventBits_t bits = xEventGroupWaitBits(
        ctx.events, BIT_PUB_DONE | BIT_FAILED,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));

    esp_mqtt_client_stop(cli);
    esp_mqtt_client_destroy(cli);
    vEventGroupDelete(ctx.events);

    if (bits & BIT_PUB_DONE) return ESP_OK;
    if (bits & BIT_FAILED)   return ESP_FAIL;
    return ESP_ERR_TIMEOUT;
}
