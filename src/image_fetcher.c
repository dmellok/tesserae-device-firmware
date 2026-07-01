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
} dl_ctx_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    dl_ctx_t *ctx = e->user_data;

    switch (e->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(e->header_key, "Content-Type") == 0) {
            strncpy(ctx->out->content_type, e->header_value,
                    sizeof(ctx->out->content_type) - 1);
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

esp_err_t image_fetch(const char *url, fetched_image_t *out)
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
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "downloaded %u bytes (type=%s)",
             (unsigned)out->len, out->content_type[0] ? out->content_type : "?");
    return ESP_OK;
}
