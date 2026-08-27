#include "image_decoder.h"
#include "app_config.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "decode";

/* v1 wire format: panel-native 4-bpp packed buffer, exactly EPD_BUF_BYTES.
 * Tesserae's esp32_bin renderer guarantees this length; any deviation means
 * either a half-downloaded body or a server bug, and feeding garbage to the
 * panel costs ~30 s of refresh power for a useless render. Hard-fail. */
esp_err_t image_decode_to_frame(fetched_image_t *src,
                                const char *url,
                                uint8_t **out_frame)
{
    (void)url;
    if (!src || !out_frame) return ESP_ERR_INVALID_ARG;
    *out_frame = NULL;

    if (src->len != EPD_BUF_BYTES) {
        ESP_LOGE(TAG,
            "frame size mismatch: got %u bytes, expected %u "
            "(content-type='%s'); refusing to paint",
            (unsigned)src->len, (unsigned)EPD_BUF_BYTES,
            src->content_type);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "raw panel-native frame (%u bytes)", (unsigned)src->len);
    /* Hand the download buffer over rather than copying it. Both live in
     * TESSERAE_FB_CAPS memory, and holding two EPD_BUF_BYTES blocks at once
     * doubled the peak: on the no-PSRAM C3 with an SD card mounted (Xteink
     * X4) the second allocation failed, and the old NO_MEM return was the
     * one exit with no log line -- the frame downloaded, decoded, then
     * silently never painted. */
    *out_frame = src->data;
    src->data = NULL;
    src->len = 0;
    return ESP_OK;
}
