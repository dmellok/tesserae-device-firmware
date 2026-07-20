/* ota_verify.c -- signed OTA descriptor verification. */

#include "ota_verify.h"

#include <string.h>

#include <optional/monocypher-ed25519.h>

typedef struct {
    const char *id;
    uint8_t public_key[OTA_ED25519_PUBLIC_BYTES];
} trusted_key_t;

/* Published by Tesserae's production signer.  Public verification material,
 * not a secret.  Keep keys indexed by id so prod-2 can overlap prod-1 during
 * rotation without reflashing every device in one step. */
static const trusted_key_t s_trusted_keys[] = {
    {
        .id = "prod-1",
        .public_key = {
            0x97, 0xf3, 0x29, 0x4e, 0x7b, 0xf8, 0xf2, 0xdf,
            0xb9, 0x65, 0xac, 0x27, 0x39, 0xe6, 0xd2, 0xd7,
            0xee, 0x34, 0x6c, 0x42, 0x68, 0x1a, 0x94, 0xd8,
            0x7c, 0x91, 0x89, 0x94, 0x2d, 0xb3, 0x96, 0x5e,
        },
    },
#if defined(TESSERAE_OTA_TRUST_TEST_KEY) && TESSERAE_OTA_TRUST_TEST_KEY
    {
        .id = "test-ed25519-1",
        .public_key = {
            0x03, 0xa1, 0x07, 0xbf, 0xf3, 0xce, 0x10, 0xbe,
            0x1d, 0x70, 0xdd, 0x18, 0xe7, 0x4b, 0xc0, 0x99,
            0x67, 0xe4, 0xd6, 0x30, 0x9b, 0xa5, 0x0d, 0x5f,
            0x1d, 0xdc, 0x86, 0x64, 0x12, 0x55, 0x31, 0xb8,
        },
    },
#endif
};

static int b64u_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

/* Strict RFC 4648 base64url without padding.  Reject non-canonical unused
 * bits as well as '=', whitespace, and impossible one-character tails. */
static bool b64u_decode(const char *input, uint8_t *output, size_t capacity,
                        size_t *output_len)
{
    if (input == NULL || output == NULL || output_len == NULL) return false;

    size_t n = strlen(input);
    if ((n & 3u) == 1u) return false;

    size_t i = 0;
    size_t o = 0;
    while (n - i >= 4) {
        int a = b64u_value(input[i]);
        int b = b64u_value(input[i + 1]);
        int c = b64u_value(input[i + 2]);
        int d = b64u_value(input[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || o + 3 > capacity) return false;
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12)
                   | ((uint32_t)c << 6) | (uint32_t)d;
        output[o++] = (uint8_t)(v >> 16);
        output[o++] = (uint8_t)(v >> 8);
        output[o++] = (uint8_t)v;
        i += 4;
    }

    size_t tail = n - i;
    if (tail == 2) {
        int a = b64u_value(input[i]);
        int b = b64u_value(input[i + 1]);
        if (a < 0 || b < 0 || (b & 0x0f) != 0 || o + 1 > capacity) return false;
        output[o++] = (uint8_t)(((uint32_t)a << 2) | ((uint32_t)b >> 4));
    } else if (tail == 3) {
        int a = b64u_value(input[i]);
        int b = b64u_value(input[i + 1]);
        int c = b64u_value(input[i + 2]);
        if (a < 0 || b < 0 || c < 0 || (c & 0x03) != 0 || o + 2 > capacity)
            return false;
        uint32_t v = ((uint32_t)a << 10) | ((uint32_t)b << 4) | ((uint32_t)c >> 2);
        output[o++] = (uint8_t)(v >> 8);
        output[o++] = (uint8_t)v;
    }

    *output_len = o;
    return true;
}

bool ota_ed25519_verify(const uint8_t public_key[OTA_ED25519_PUBLIC_BYTES],
                        const uint8_t *message, size_t message_len,
                        const uint8_t signature[OTA_ED25519_SIG_BYTES])
{
    if (public_key == NULL || message == NULL || signature == NULL) return false;
    return crypto_ed25519_check(signature, public_key, message, message_len) == 0;
}

ota_verify_reason_t ota_verify_signed_payload(const char *payload_b64u,
                                              const char *signature_b64u,
                                              ota_verified_payload_t *out)
{
    if (out == NULL) return OTA_VERIFY_MALFORMED_DESCRIPTOR;
    memset(out, 0, sizeof *out);

    uint8_t signature[OTA_ED25519_SIG_BYTES];
    size_t signature_len = 0;
    if (!b64u_decode(payload_b64u, out->bytes, OTA_MAX_PAYLOAD_BYTES, &out->length)
        || !b64u_decode(signature_b64u, signature, sizeof signature, &signature_len)
        || signature_len != OTA_ED25519_SIG_BYTES) {
        memset(out, 0, sizeof *out);
        return OTA_VERIFY_MALFORMED_DESCRIPTOR;
    }

    for (size_t i = 0; i < sizeof s_trusted_keys / sizeof s_trusted_keys[0]; i++) {
        if (ota_ed25519_verify(s_trusted_keys[i].public_key, out->bytes, out->length,
                               signature)) {
            out->bytes[out->length] = '\0';
            out->verified_key_id = s_trusted_keys[i].id;
            return OTA_VERIFY_OK;
        }
    }

    memset(out, 0, sizeof *out);
    return OTA_VERIFY_BAD_SIGNATURE;
}

const char *ota_verify_reason_name(ota_verify_reason_t reason)
{
    switch (reason) {
    case OTA_VERIFY_OK:                   return "ok";
    case OTA_VERIFY_MALFORMED_DESCRIPTOR: return "malformed_descriptor";
    case OTA_VERIFY_BAD_SIGNATURE:        return "bad_signature";
    case OTA_VERIFY_MALFORMED_MANIFEST:   return "malformed_manifest";
    case OTA_VERIFY_SCHEMA_VERSION:       return "schema_version";
    case OTA_VERIFY_KIND_MISMATCH:        return "kind_mismatch";
    case OTA_VERIFY_ALREADY_CURRENT:      return "already_current";
    case OTA_VERIFY_SIZE_MISMATCH:        return "size_mismatch";
    case OTA_VERIFY_DIGEST_MISMATCH:      return "digest_mismatch";
    default:                              return "malformed_descriptor";
    }
}
