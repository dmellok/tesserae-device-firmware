/* ota_manifest.h -- parse and target-check an authenticated OTA manifest. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ota_verify.h"

#define OTA_KEY_ID_MAX       31
#define OTA_DEVICE_KIND_MAX  31
#define OTA_FW_VERSION_MAX   31
#define OTA_IMAGE_URL_MAX   511
#define OTA_SHA256_BYTES      32

typedef struct {
    char key_id[OTA_KEY_ID_MAX + 1];
    char device_kind[OTA_DEVICE_KIND_MAX + 1];
    char fw_version[OTA_FW_VERSION_MAX + 1];
    char image_url[OTA_IMAGE_URL_MAX + 1];
    uint32_t size_bytes;
    uint8_t sha256[OTA_SHA256_BYTES];
} ota_manifest_t;

/* Parse only bytes already authenticated by ota_verify_signed_payload(), then
 * enforce manifest shape, schema, signing-key identity, target kind, and the
 * already-running no-op check. Image size/hash are checked by the streaming
 * installer in a later phase. */
ota_verify_reason_t ota_manifest_parse_and_check(
    const ota_verified_payload_t *verified,
    const char *expected_kind,
    const char *current_fw,
    ota_manifest_t *out);

/* Convenience entry point for a wire descriptor. The decoded payload uses
 * temporary heap storage so the main task does not spend ~1.5 KiB of stack. */
ota_verify_reason_t ota_descriptor_check(
    const char *payload_b64u,
    const char *signature_b64u,
    const char *expected_kind,
    const char *current_fw,
    ota_manifest_t *out);
