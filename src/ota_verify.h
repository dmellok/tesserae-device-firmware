/*
 * ota_verify.h -- trust anchors and signature-first verification for OTA.
 *
 * Mirrors Tesserae docs/ota/contract.md.  The descriptor carries base64url
 * payload + signature strings; we decode both, try the trusted Ed25519 keys,
 * and return the raw payload only after a signature succeeds.  JSON parsing is
 * intentionally a later step so unauthenticated bytes never reach cJSON.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_SCHEMA_VERSION       1
#define OTA_ED25519_PUBLIC_BYTES 32
#define OTA_ED25519_SIG_BYTES    64
#define OTA_MAX_PAYLOAD_BYTES    1536
#define OTA_MAX_PAYLOAD_B64URL   ((OTA_MAX_PAYLOAD_BYTES * 4 + 2) / 3)
#define OTA_ED25519_SIG_B64URL   ((OTA_ED25519_SIG_BYTES * 4 + 2) / 3)

typedef enum {
    OTA_VERIFY_OK = 0,
    OTA_VERIFY_MALFORMED_DESCRIPTOR,
    OTA_VERIFY_BAD_SIGNATURE,
    OTA_VERIFY_MALFORMED_MANIFEST,
    OTA_VERIFY_SCHEMA_VERSION,
    OTA_VERIFY_KIND_MISMATCH,
    OTA_VERIFY_ALREADY_CURRENT,
    OTA_VERIFY_SIZE_MISMATCH,
    OTA_VERIFY_DIGEST_MISMATCH,
} ota_verify_reason_t;

typedef struct {
    uint8_t bytes[OTA_MAX_PAYLOAD_BYTES + 1];
    size_t length;
    /* Key whose signature succeeded.  Later manifest parsing must require the
     * signed key_id field to match this value. */
    const char *verified_key_id;
} ota_verified_payload_t;

/* Verify a descriptor against the firmware trust set.  Production builds
 * trust prod-1 only.  Defining TESSERAE_OTA_TRUST_TEST_KEY=1 adds the published
 * test-ed25519-1 key for host/integration tests; release builds never set it. */
ota_verify_reason_t ota_verify_signed_payload(const char *payload_b64u,
                                              const char *signature_b64u,
                                              ota_verified_payload_t *out);

/* Low-level helper used by RFC 8032 host vectors.  Returns true on success. */
bool ota_ed25519_verify(const uint8_t public_key[OTA_ED25519_PUBLIC_BYTES],
                        const uint8_t *message, size_t message_len,
                        const uint8_t signature[OTA_ED25519_SIG_BYTES]);

const char *ota_verify_reason_name(ota_verify_reason_t reason);
