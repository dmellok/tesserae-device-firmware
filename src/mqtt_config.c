#include "mqtt_config.h"
#include "app_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "mqtt_cfg";

static void load_str(nvs_handle_t h, const char *key,
                     char *dst, size_t dst_sz, const char *fallback)
{
    size_t len = dst_sz;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err != ESP_OK) {
        strncpy(dst, fallback ? fallback : "", dst_sz - 1);
        dst[dst_sz - 1] = '\0';
    }
}

/* esp-mqtt's uri parser rejects bare host:port; users typing the broker into
 * the captive portal routinely leave the scheme off. Prepend mqtt:// in-place
 * if none of the recognized schemes is present. */
static void normalize_mqtt_uri(char *uri, size_t uri_sz)
{
    if (!uri || !uri[0]) return;
    if (strncmp(uri, "mqtt://",  7) == 0) return;
    if (strncmp(uri, "mqtts://", 8) == 0) return;
    if (strncmp(uri, "ws://",    5) == 0) return;
    if (strncmp(uri, "wss://",   6) == 0) return;

    const char prefix[] = "mqtt://";
    size_t plen = sizeof(prefix) - 1;
    size_t ulen = strlen(uri);
    if (ulen + plen + 1 > uri_sz) return;   /* no room; let esp-mqtt log the error */
    memmove(uri + plen, uri, ulen + 1);
    memcpy(uri, prefix, plen);
}

bool mqtt_device_id_valid(const char *id)
{
    if (!id) return false;
    size_t n = strlen(id);
    if (n < DEVICE_ID_MIN_LEN || n > DEVICE_ID_MAX_LEN) return false;
    if (id[0] < 'a' || id[0] > 'z') return false;   /* must start with a letter */
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

/* Pull "<X>" out of a legacy "tesserae/<X>/frame/bin" topic. Returns false for
 * anything that doesn't match (including the very old "inky/esp32/update"),
 * leaving the caller to fall back to the default device_id. */
static bool device_id_from_legacy_topic(const char *topic, char *out, size_t out_sz)
{
    const char prefix[] = "tesserae/";
    const char suffix[] = "/frame/bin";
    size_t plen = sizeof(prefix) - 1;
    if (strncmp(topic, prefix, plen) != 0) return false;

    const char *id_start = topic + plen;
    const char *id_end = strstr(id_start, suffix);
    if (!id_end || id_end <= id_start) return false;

    size_t idlen = (size_t)(id_end - id_start);
    if (idlen + 1 > out_sz) return false;
    memcpy(out, id_start, idlen);
    out[idlen] = '\0';
    return mqtt_device_id_valid(out);
}

void mqtt_config_load(mqtt_config_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    /* READWRITE because the one-shot legacy->device_id migration may persist. */
    if (nvs_open(NVS_NS_MQTT, NVS_READWRITE, &h) != ESP_OK) {
        /* Nothing stored yet -- use compile-time defaults across the board. */
        strncpy(out->uri,       MQTT_DEFAULT_URI,       sizeof(out->uri)       - 1);
        strncpy(out->device_id, MQTT_DEFAULT_DEVICE_ID, sizeof(out->device_id) - 1);
        strncpy(out->user,      MQTT_DEFAULT_USER,      sizeof(out->user)      - 1);
        strncpy(out->pass,      MQTT_DEFAULT_PASS,      sizeof(out->pass)      - 1);
        normalize_mqtt_uri(out->uri, sizeof(out->uri));
        return;
    }

    load_str(h, NVS_KEY_MQTT_URI,  out->uri,  sizeof(out->uri),  MQTT_DEFAULT_URI);
    load_str(h, NVS_KEY_MQTT_USER, out->user, sizeof(out->user), MQTT_DEFAULT_USER);
    load_str(h, NVS_KEY_MQTT_PASS, out->pass, sizeof(out->pass), MQTT_DEFAULT_PASS);

    /* device_id: the new key wins; otherwise migrate the legacy topic key once;
     * otherwise fall back to the default. */
    size_t len = sizeof(out->device_id);
    if (nvs_get_str(h, NVS_KEY_MQTT_DEVICE_ID, out->device_id, &len) != ESP_OK) {
        out->device_id[0] = '\0';
        char legacy[96] = {0};
        size_t llen = sizeof(legacy);
        bool from_legacy = false;
        if (nvs_get_str(h, NVS_KEY_MQTT_TOPIC, legacy, &llen) == ESP_OK &&
            device_id_from_legacy_topic(legacy, out->device_id, sizeof(out->device_id))) {
            from_legacy = true;
        }
        if (out->device_id[0] == '\0') {
            strncpy(out->device_id, MQTT_DEFAULT_DEVICE_ID, sizeof(out->device_id) - 1);
        }
        /* Persist the resolved id and drop the legacy key so this only runs once. */
        nvs_set_str(h, NVS_KEY_MQTT_DEVICE_ID, out->device_id);
        nvs_erase_key(h, NVS_KEY_MQTT_TOPIC);   /* ESP_ERR_NVS_NOT_FOUND is fine */
        nvs_commit(h);
        ESP_LOGI(TAG, "device_id initialized to '%s' (%s)",
                 out->device_id, from_legacy ? "migrated from legacy topic" : "default");
    }

    nvs_close(h);

    normalize_mqtt_uri(out->uri, sizeof(out->uri));

    ESP_LOGI(TAG, "loaded uri='%s' device_id='%s' user='%s'",
             out->uri, out->device_id, out->user[0] ? out->user : "(none)");
}

esp_err_t mqtt_config_save(const char *uri, const char *device_id,
                           const char *user, const char *pass)
{
    if (!uri || !*uri) return ESP_ERR_INVALID_ARG;
    if (!mqtt_device_id_valid(device_id)) return ESP_ERR_INVALID_ARG;

    char norm_uri[160];
    strncpy(norm_uri, uri, sizeof(norm_uri) - 1);
    norm_uri[sizeof(norm_uri) - 1] = '\0';
    normalize_mqtt_uri(norm_uri, sizeof(norm_uri));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_MQTT, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, NVS_KEY_MQTT_URI, norm_uri);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_MQTT_DEVICE_ID, device_id);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_MQTT_USER, user ? user : "");
    if (err == ESP_OK) {
        if (pass) {
            err = nvs_set_str(h, NVS_KEY_MQTT_PASS, pass);
        } else {
            /* pass == NULL: keep stored broker password (blank form field). */
            size_t l = 0;
            if (nvs_get_str(h, NVS_KEY_MQTT_PASS, NULL, &l) == ESP_ERR_NVS_NOT_FOUND) {
                err = nvs_set_str(h, NVS_KEY_MQTT_PASS, "");
            }
        }
    }
    if (err == ESP_OK) nvs_erase_key(h, NVS_KEY_MQTT_TOPIC);   /* drop legacy if present */
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
