#include "image_fetcher.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "fetch";

typedef struct {
    fetched_image_t *out;
    size_t cap;          /* current buffer capacity */
    bool   oom;          /* set if any realloc fails */
    char  *etag;         /* optional: response ETag, quotes stripped */
    size_t etag_cap;
} dl_ctx_t;

/* Copy an ETag header value, dropping the surrounding quotes and any weak
 * validator prefix, so callers can round-trip it into If-None-Match without
 * caring how the origin formatted it. */
static void copy_etag(dl_ctx_t *ctx, const char *value)
{
    if (!ctx->etag || !ctx->etag_cap || !value) return;
    if (strncasecmp(value, "W/", 2) == 0) value += 2;
    size_t n = strlen(value);
    if (n >= 2 && value[0] == '"' && value[n - 1] == '"') { value++; n -= 2; }
    if (n >= ctx->etag_cap) n = ctx->etag_cap - 1;
    memcpy(ctx->etag, value, n);
    ctx->etag[n] = '\0';
}

static esp_err_t on_http(esp_http_client_event_t *e)
{
    dl_ctx_t *ctx = e->user_data;

    switch (e->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(e->header_key, "Content-Type") == 0) {
            strncpy(ctx->out->content_type, e->header_value,
                    sizeof(ctx->out->content_type) - 1);
        } else if (strcasecmp(e->header_key, "ETag") == 0) {
            copy_etag(ctx, e->header_value);
        }
        break;

    case HTTP_EVENT_ON_DATA: {
        if (ctx->oom) return ESP_FAIL;

        size_t need = ctx->out->len + e->data_len;
        if (need > IMAGE_FETCH_MAX_BYTES) {
            ESP_LOGE(TAG, "response would exceed %u-byte cap",
                     (unsigned)IMAGE_FETCH_MAX_BYTES);
            ctx->oom = true;
            return ESP_FAIL;
        }

        /* Grow geometrically; start at 64KB. */
        if (need > ctx->cap) {
            size_t new_cap = ctx->cap ? ctx->cap : 65536;
            while (new_cap < need) new_cap *= 2;
            if (new_cap > IMAGE_FETCH_MAX_BYTES) new_cap = IMAGE_FETCH_MAX_BYTES;

            uint8_t *grown = heap_caps_realloc(ctx->out->data, new_cap, MALLOC_CAP_SPIRAM);
            if (!grown) {
                ESP_LOGE(TAG, "PSRAM realloc(%u) failed", (unsigned)new_cap);
                ctx->oom = true;
                return ESP_FAIL;
            }
            ctx->out->data = grown;
            ctx->cap = new_cap;
        }
        memcpy(ctx->out->data + ctx->out->len, e->data, e->data_len);
        ctx->out->len = need;
        break;
    }

    default:
        break;
    }
    return ESP_OK;
}

esp_err_t image_fetch_conditional(const char *url, const char *bearer_token,
                                  const char *etag_in,
                                  char *etag_out, size_t etag_out_cap,
                                  int *status_out, fetched_image_t *out)
{
    if (!url || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (etag_out && etag_out_cap) etag_out[0] = '\0';
    if (status_out) *status_out = 0;

    dl_ctx_t ctx = { .out = out, .cap = 0, .oom = false,
                     .etag = etag_out, .etag_cap = etag_out_cap };

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    if (bearer_token && bearer_token[0]) {
        char auth[300];
        snprintf(auth, sizeof auth, "Bearer %s", bearer_token);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    if (etag_in && etag_in[0]) {
        char inm[96];
        snprintf(inm, sizeof inm, "\"%s\"", etag_in);
        esp_http_client_set_header(cli, "If-None-Match", inm);
    }

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (status_out) *status_out = status;

    if (ctx.oom) {
        free(out->data);
        memset(out, 0, sizeof(*out));
        return ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "perform failed: %s", esp_err_to_name(err));
        free(out->data);
        memset(out, 0, sizeof(*out));
        return err;
    }
    /* 304/204 are normal relay outcomes ("unchanged" / "nothing yet"), not
     * errors: report ESP_OK with an empty body and let the caller branch on
     * the status code. */
    if (status == 304 || status == 204) {
        free(out->data);
        memset(out, 0, sizeof(*out));
        return ESP_OK;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "http status %d", status);
        free(out->data);
        memset(out, 0, sizeof(*out));
        return (status == 404) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    ESP_LOGI(TAG, "downloaded %u bytes (type=%s)",
             (unsigned)out->len, out->content_type[0] ? out->content_type : "?");
    return ESP_OK;
}

esp_err_t image_fetch_auth(const char *url, const char *bearer_token,
                           fetched_image_t *out)
{
    if (!url || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    dl_ctx_t ctx = { .out = out, .cap = 0, .oom = false };

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    if (bearer_token && bearer_token[0]) {
        char auth[300];
        snprintf(auth, sizeof auth, "Bearer %s", bearer_token);
        esp_http_client_set_header(cli, "Authorization", auth);
    }

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (ctx.oom) {
        if (out->data) free(out->data);
        memset(out, 0, sizeof(*out));
        return ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "perform failed: %s", esp_err_to_name(err));
        if (out->data) free(out->data);
        memset(out, 0, sizeof(*out));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "http status %d", status);
        if (out->data) free(out->data);
        memset(out, 0, sizeof(*out));
        /* 404 is a contract signal on deck frame URLs (stale manifest ->
         * caller re-fetches the manifest), so keep it distinguishable. */
        return (status == 404) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    ESP_LOGI(TAG, "downloaded %u bytes (type=%s)",
             (unsigned)out->len, out->content_type[0] ? out->content_type : "?");
    return ESP_OK;
}

esp_err_t image_fetch(const char *url, fetched_image_t *out)
{
    return image_fetch_auth(url, NULL, out);
}
