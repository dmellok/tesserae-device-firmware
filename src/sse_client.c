/* sse_client.c -- proto2 SSE push transport. See sse_client.h. */

#include "sse_client.h"

#if defined(BOARD_OVERLAY_PARTIAL) && defined(BOARD_HAS_TOUCH)

#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "overlay_run.h"
#include "proto2_run.h"
#include "rest_config.h"
#include "touch3_run.h"

static const char *TAG = "sse";

#define SSE_LINE_MAX   2048     /* one field line (patch docs ~1.5 KB) */
#define SSE_SILENCE_US (60LL * 1000 * 1000)
#define SSE_BACKOFF_MIN_S 1
#define SSE_BACKOFF_MAX_S 60

static esp_http_client_handle_t s_cli;
static bool     s_open;
static int64_t  s_last_rx_us;
static int      s_backoff_s = SSE_BACKOFF_MIN_S;
static int64_t  s_next_try_us;

/* Line assembly + current event state. */
static char s_line[SSE_LINE_MAX];
static int  s_line_len;
static char s_event[24];
static char s_data[SSE_LINE_MAX];
static int  s_data_len;

static void sse_close(void)
{
    if (s_cli) {
        esp_http_client_close(s_cli);
        esp_http_client_cleanup(s_cli);
        s_cli = NULL;
    }
    s_open = false;
    s_event[0] = '\0';
    s_data_len = 0;
    s_line_len = 0;
}

void sse_stop(void)
{
    sse_close();
    s_backoff_s = SSE_BACKOFF_MIN_S;
    s_next_try_us = 0;
}

bool sse_connected(void) { return s_open; }

static void dispatch_event(void)
{
    if (!s_data_len) return;
    s_data[s_data_len] = '\0';
    if (strcmp(s_event, "values") == 0) {
        overlay_ingest_values(s_data, (size_t)s_data_len);
        proto2_ingest_values(s_data, (size_t)s_data_len);
        /* Touch v3 shares this envelope: values are keyed by value_key, and v3
         * maps them onto its primitives. It is v3's ONLY reconcile path --
         * /interact replies carry no confirmed state. */
        touch3_ingest_values(s_data, (size_t)s_data_len);
    } else if (strcmp(s_event, "patches") == 0) {
        overlay_ingest_patches(s_data, (size_t)s_data_len);
    } else if (strcmp(s_event, "sync") == 0) {
        proto2_note_sync(s_data, (size_t)s_data_len);
    } else if (s_event[0]) {
        ESP_LOGD(TAG, "ignoring event '%s'", s_event);   /* additive */
    }
    s_event[0] = '\0';
    s_data_len = 0;
}

static void feed_line(const char *line)
{
    if (line[0] == '\0') { dispatch_event(); return; }   /* blank = fire */
    if (line[0] == ':') { return; }                      /* ":ka" keepalive */
    if (strncmp(line, "event:", 6) == 0) {
        const char *v = line + 6;
        while (*v == ' ') v++;
        snprintf(s_event, sizeof s_event, "%s", v);
    } else if (strncmp(line, "data:", 5) == 0) {
        const char *v = line + 5;
        while (*v == ' ') v++;
        int n = (int)strlen(v);
        /* Multi-line data joins with \n per the SSE spec. */
        if (s_data_len && s_data_len + 1 < (int)sizeof s_data - 1)
            s_data[s_data_len++] = '\n';
        if (s_data_len + n < (int)sizeof s_data - 1) {
            memcpy(s_data + s_data_len, v, (size_t)n);
            s_data_len += n;
        } else {
            s_data_len = 0;    /* oversize event: drop whole (strictness) */
            s_event[0] = '\0';
        }
    }
    /* other fields (id:, retry:): ignored */
}

/* Set once a v3 /frame/stream attempt came back 404: that server serves the
 * legacy /stream only, and there is no point re-probing every reconnect. */
static bool s_v3_absent;

static bool sse_open_stream(void)
{
    /* Touch v3 pins the stream at /frame/stream (contract §7); the v2 push
     * transport used /stream. Prefer the v3 path while a v3 spec is live and
     * fall back permanently on a 404, so one connection serves both. */
    bool try_v3 = touch3_active() && !s_v3_absent;
    char url[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/%s",
             rest_config_get()->server_url, rest_config_device_id(),
             try_v3 ? "frame/stream" : "stream");
    char auth[300];
    snprintf(auth, sizeof auth, "Bearer %s",
             rest_config_get()->device_token);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 250,          /* short reads: the pump must not block */
        .buffer_size = 2048,
        .keep_alive_enable = true,
    };
    s_cli = esp_http_client_init(&cfg);
    if (!s_cli) return false;
    esp_http_client_set_header(s_cli, "Authorization", auth);
    esp_http_client_set_header(s_cli, "Accept", "text/event-stream");

    esp_err_t err = esp_http_client_open(s_cli, 0);
    if (err != ESP_OK) { sse_close(); return false; }
    int64_t hdr = esp_http_client_fetch_headers(s_cli);
    int status = esp_http_client_get_status_code(s_cli);
    if (hdr < 0 || status != 200) {
        ESP_LOGI(TAG, "stream open failed (http %d)", status);
        if (try_v3 && status == 404) {
            ESP_LOGI(TAG, "no /frame/stream; using legacy /stream");
            s_v3_absent = true;      /* the next attempt takes the v2 path */
        }
        sse_close();
        return false;
    }
    s_open = true;
    s_last_rx_us = esp_timer_get_time();
    s_backoff_s = SSE_BACKOFF_MIN_S;
    ESP_LOGI(TAG, "stream connected (%s)", try_v3 ? "v3" : "legacy");
    return true;
}

static void schedule_retry(void)
{
    /* Exponential backoff with +-25 % jitter, capped at 60 s. */
    int jitter = s_backoff_s / 4;
    int delay = s_backoff_s + (jitter ? (int)(esp_random() % (2 * jitter + 1))
                                        - jitter : 0);
    if (delay < 1) delay = 1;
    s_next_try_us = esp_timer_get_time() + (int64_t)delay * 1000000;
    s_backoff_s = s_backoff_s * 2;
    if (s_backoff_s > SSE_BACKOFF_MAX_S) s_backoff_s = SSE_BACKOFF_MAX_S;
}

bool sse_pump(int read_budget_ms)
{
    int64_t now = esp_timer_get_time();

    if (!s_open) {
        if (now < s_next_try_us) return true;   /* backing off normally */
        if (!sse_open_stream()) { schedule_retry(); return true; }
    }

    /* Silence watchdog: keepalives come every 25 s. */
    if (now - s_last_rx_us > SSE_SILENCE_US) {
        ESP_LOGW(TAG, "stream silent > 60 s; reconnecting");
        sse_close();
        schedule_retry();
        return true;
    }

    int64_t deadline = now + (int64_t)read_budget_ms * 1000;
    char chunk[256];
    while (esp_timer_get_time() < deadline) {
        int n = esp_http_client_read(s_cli, chunk, sizeof chunk);
        if (n < 0) {                 /* connection dropped */
            ESP_LOGI(TAG, "stream read error; reconnecting");
            sse_close();
            schedule_retry();
            return true;
        }
        if (n == 0) break;           /* nothing buffered right now */
        s_last_rx_us = esp_timer_get_time();
        for (int i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\r') continue;
            if (c == '\n') {
                s_line[s_line_len] = '\0';
                feed_line(s_line);
                s_line_len = 0;
            } else if (s_line_len < SSE_LINE_MAX - 1) {
                s_line[s_line_len++] = c;
            }
        }
    }
    return true;
}

#endif /* BOARD_OVERLAY_PARTIAL && BOARD_HAS_TOUCH */
