/* ota_manifest.c -- authenticated OTA manifest parsing and target gates. */

#include "ota_manifest.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static bool copy_json_string(const cJSON *root, const char *name,
                             char *out, size_t capacity)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
    size_t length = strlen(item->valuestring);
    if (length == 0 || length >= capacity) return false;
    memcpy(out, item->valuestring, length + 1);
    return true;
}

static bool decode_lower_hex_sha256(const cJSON *root,
                                    uint8_t out[OTA_SHA256_BYTES])
{
    static const char hex[] = "0123456789abcdef";
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) != OTA_SHA256_BYTES * 2) return false;

    for (size_t i = 0; i < OTA_SHA256_BYTES; i++) {
        const char *hi = strchr(hex, item->valuestring[i * 2]);
        const char *lo = strchr(hex, item->valuestring[i * 2 + 1]);
        if (hi == NULL || lo == NULL) return false;
        out[i] = (uint8_t)(((hi - hex) << 4) | (lo - hex));
    }
    return true;
}

static bool json_u32(const cJSON *root, const char *name, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 1.0 || item->valuedouble > UINT32_MAX ||
        floor(item->valuedouble) != item->valuedouble) return false;
    *out = (uint32_t)item->valuedouble;
    return true;
}

static bool trailing_json_space_only(const char *end, const char *limit)
{
    if (end == NULL || end > limit) return false;
    while (end < limit) {
        unsigned char c = (unsigned char)*end++;
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    }
    return true;
}

ota_verify_reason_t ota_manifest_parse_and_check(
    const ota_verified_payload_t *verified,
    const char *expected_kind,
    const char *current_fw,
    ota_manifest_t *out)
{
    if (verified == NULL || verified->verified_key_id == NULL || out == NULL ||
        verified->length == 0 || verified->length > OTA_MAX_PAYLOAD_BYTES)
        return OTA_VERIFY_MALFORMED_MANIFEST;

    memset(out, 0, sizeof *out);
    const char *begin = (const char *)verified->bytes;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(begin, verified->length, &end, false);
    if (root == NULL || !cJSON_IsObject(root) ||
        !trailing_json_space_only(end, begin + verified->length)) {
        cJSON_Delete(root);
        return OTA_VERIFY_MALFORMED_MANIFEST;
    }

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(schema) || !isfinite(schema->valuedouble) ||
        floor(schema->valuedouble) != schema->valuedouble) {
        cJSON_Delete(root);
        return OTA_VERIFY_MALFORMED_MANIFEST;
    }
    if (schema->valuedouble != OTA_SCHEMA_VERSION) {
        cJSON_Delete(root);
        return OTA_VERIFY_SCHEMA_VERSION;
    }

    bool shape_ok = copy_json_string(root, "key_id", out->key_id,
                                     sizeof out->key_id) &&
                    copy_json_string(root, "device_kind", out->device_kind,
                                     sizeof out->device_kind) &&
                    copy_json_string(root, "fw_version", out->fw_version,
                                     sizeof out->fw_version) &&
                    copy_json_string(root, "image_url", out->image_url,
                                     sizeof out->image_url) &&
                    json_u32(root, "size_bytes", &out->size_bytes) &&
                    decode_lower_hex_sha256(root, out->sha256);
    cJSON_Delete(root);
    if (!shape_ok) {
        memset(out, 0, sizeof *out);
        return OTA_VERIFY_MALFORMED_MANIFEST;
    }

    /* The trust set tries keys before JSON parsing. Bind the signed key_id to
     * whichever trusted public key actually verified the bytes. */
    if (strcmp(out->key_id, verified->verified_key_id) != 0) {
        memset(out, 0, sizeof *out);
        return OTA_VERIFY_BAD_SIGNATURE;
    }
    if (expected_kind != NULL && strcmp(out->device_kind, expected_kind) != 0)
        return OTA_VERIFY_KIND_MISMATCH;
    if (current_fw != NULL && strcmp(out->fw_version, current_fw) == 0)
        return OTA_VERIFY_ALREADY_CURRENT;
    return OTA_VERIFY_OK;
}

ota_verify_reason_t ota_descriptor_check(
    const char *payload_b64u,
    const char *signature_b64u,
    const char *expected_kind,
    const char *current_fw,
    ota_manifest_t *out)
{
    if (out == NULL) return OTA_VERIFY_MALFORMED_DESCRIPTOR;
    memset(out, 0, sizeof *out);

    ota_verified_payload_t *verified = calloc(1, sizeof *verified);
    if (verified == NULL) return OTA_VERIFY_MALFORMED_DESCRIPTOR;
    ota_verify_reason_t reason = ota_verify_signed_payload(payload_b64u,
                                                           signature_b64u,
                                                           verified);
    if (reason == OTA_VERIFY_OK)
        reason = ota_manifest_parse_and_check(verified, expected_kind,
                                              current_fw, out);
    free(verified);
    return reason;
}
