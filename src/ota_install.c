/* ota_install.c -- bounded HTTPS/HTTP download into the inactive OTA slot. */

#include "ota_install.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "battery.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"

static const char *TAG = "ota_install";

#define OTA_DOWNLOAD_BUFFER_BYTES 8192
#define OTA_DOWNLOAD_TIMEOUT_MS  30000

static void close_client(esp_http_client_handle_t client)
{
    if (client == NULL) return;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

ota_install_result_t ota_install_apply(const ota_manifest_t *manifest)
{
    if (manifest == NULL || manifest->image_url[0] == '\0' ||
        manifest->size_bytes == 0) return OTA_INSTALL_INVALID_ARGUMENT;

    int battery_mv = battery_read_mv();
    if (battery_mv < TESSERAE_OTA_MIN_BATTERY_MV) {
        ESP_LOGW(TAG, "update deferred: battery=%d mV, minimum=%d mV",
                 battery_mv, TESSERAE_OTA_MIN_BATTERY_MV);
        return OTA_INSTALL_LOW_BATTERY;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (running == NULL || target == NULL || target == running ||
        target->type != ESP_PARTITION_TYPE_APP) return OTA_INSTALL_NO_UPDATE_SLOT;
    if (manifest->size_bytes > target->size) return OTA_INSTALL_IMAGE_TOO_LARGE;

    esp_http_client_config_t cfg = {
        .url = manifest->image_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_DOWNLOAD_TIMEOUT_MS,
        .buffer_size = OTA_DOWNLOAD_BUFFER_BYTES,
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
    };
    if (strncmp(manifest->image_url, "https://", 8) == 0)
        cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return OTA_INSTALL_HTTP_INIT_FAILED;

    char user_agent[96];
    snprintf(user_agent, sizeof user_agent, "Tesserae-ESP32/%s (%s)",
             FW_VERSION, TESSERAE_DEVICE_MODEL);
    esp_http_client_set_header(client, "User-Agent", user_agent);
    esp_http_client_set_header(client, "Accept", "application/octet-stream");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        close_client(client);
        return OTA_INSTALL_HTTP_FAILED;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (content_length < 0) {
        ESP_LOGW(TAG, "response has no usable Content-Length");
        close_client(client);
        return OTA_INSTALL_HTTP_FAILED;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "download returned HTTP %d", status);
        close_client(client);
        return OTA_INSTALL_HTTP_STATUS;
    }
    if ((uint64_t)content_length != manifest->size_bytes) {
        ESP_LOGW(TAG, "Content-Length mismatch: expected=%"PRIu32" got=%"PRId64,
                 manifest->size_bytes, content_length);
        close_client(client);
        return OTA_INSTALL_SIZE_MISMATCH;
    }

    uint8_t *buffer = malloc(OTA_DOWNLOAD_BUFFER_BYTES);
    if (buffer == NULL) {
        close_client(client);
        return OTA_INSTALL_NO_MEMORY;
    }

    esp_ota_handle_t ota = 0;
    bool ota_started = false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0) {
        mbedtls_sha256_free(&sha);
        free(buffer);
        close_client(client);
        return OTA_INSTALL_FLASH_WRITE_FAILED;
    }

    err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        mbedtls_sha256_free(&sha);
        free(buffer);
        close_client(client);
        return OTA_INSTALL_FLASH_WRITE_FAILED;
    }
    ota_started = true;

    ESP_LOGI(TAG, "downloading %s (%"PRIu32" bytes) to %s at 0x%"PRIx32,
             manifest->fw_version, manifest->size_bytes,
             target->label, target->address);

    ota_install_result_t result = OTA_INSTALL_APPLIED;
    uint32_t received = 0;
    while (received < manifest->size_bytes) {
        int count = esp_http_client_read(client, (char *)buffer,
                                         OTA_DOWNLOAD_BUFFER_BYTES);
        if (count < 0) {
            ESP_LOGW(TAG, "download read failed after %"PRIu32" bytes", received);
            result = OTA_INSTALL_HTTP_FAILED;
            break;
        }
        if (count == 0) {
            result = OTA_INSTALL_SIZE_MISMATCH;
            break;
        }
        if ((uint32_t)count > manifest->size_bytes - received) {
            result = OTA_INSTALL_SIZE_MISMATCH;
            break;
        }
        if (mbedtls_sha256_update(&sha, buffer, (size_t)count) != 0 ||
            esp_ota_write(ota, buffer, (size_t)count) != ESP_OK) {
            result = OTA_INSTALL_FLASH_WRITE_FAILED;
            break;
        }
        received += (uint32_t)count;
    }

    uint8_t digest[OTA_SHA256_BYTES] = {0};
    if (result == OTA_INSTALL_APPLIED &&
        mbedtls_sha256_finish(&sha, digest) != 0)
        result = OTA_INSTALL_FLASH_WRITE_FAILED;
    mbedtls_sha256_free(&sha);

    if (result == OTA_INSTALL_APPLIED && received != manifest->size_bytes)
        result = OTA_INSTALL_SIZE_MISMATCH;
    if (result == OTA_INSTALL_APPLIED &&
        memcmp(digest, manifest->sha256, sizeof digest) != 0)
        result = OTA_INSTALL_DIGEST_MISMATCH;

    free(buffer);
    close_client(client);

    if (result != OTA_INSTALL_APPLIED) {
        if (ota_started) esp_ota_abort(ota);
        ESP_LOGW(TAG, "update rejected after download: %s",
                 ota_install_result_name(result));
        return result;
    }

    err = esp_ota_end(ota);
    ota_started = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "downloaded image is invalid: %s", esp_err_to_name(err));
        return OTA_INSTALL_IMAGE_INVALID;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not select new boot partition: %s", esp_err_to_name(err));
        return OTA_INSTALL_SET_BOOT_FAILED;
    }

    ESP_LOGI(TAG, "OTA image verified and selected; reboot required");
    return OTA_INSTALL_APPLIED;
}

const char *ota_install_result_name(ota_install_result_t result)
{
    switch (result) {
    case OTA_INSTALL_APPLIED:            return "applied";
    case OTA_INSTALL_INVALID_ARGUMENT:   return "invalid_argument";
    case OTA_INSTALL_LOW_BATTERY:        return "low_battery";
    case OTA_INSTALL_NO_UPDATE_SLOT:     return "no_update_slot";
    case OTA_INSTALL_IMAGE_TOO_LARGE:    return "image_too_large";
    case OTA_INSTALL_NO_MEMORY:          return "no_memory";
    case OTA_INSTALL_HTTP_INIT_FAILED:   return "http_init_failed";
    case OTA_INSTALL_HTTP_FAILED:        return "http_failed";
    case OTA_INSTALL_HTTP_STATUS:        return "http_status";
    case OTA_INSTALL_SIZE_MISMATCH:      return "size_mismatch";
    case OTA_INSTALL_FLASH_WRITE_FAILED: return "flash_write_failed";
    case OTA_INSTALL_DIGEST_MISMATCH:    return "digest_mismatch";
    case OTA_INSTALL_IMAGE_INVALID:      return "image_invalid";
    case OTA_INSTALL_SET_BOOT_FAILED:    return "set_boot_failed";
    default:                             return "invalid_argument";
    }
}
