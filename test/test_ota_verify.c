/* Host tests for OTA signature-first verification. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ota_verify.h"
#include "ota_manifest.h"

static const char *PAYLOAD_VALID =
    "eyJkZXZpY2Vfa2luZCI6ImVzcDMyX2NsaWVudCIsImZ3X3ZlcnNpb24iOiIxLjQuMCIs"
    "ImltYWdlX3VybCI6Imh0dHBzOi8vY2RuLmV4YW1wbGUudGVzdC90ZXNzZXJhZS9lc3Az"
    "Ml9jbGllbnQvYXBwLTEuNC4wLmJpbiIsImtleV9pZCI6InRlc3QtZWQyNTUxOS0xIiwic2"
    "NoZW1hX3ZlcnNpb24iOjEsInNoYTI1NiI6ImY2Yzc1OGU4YmRlMGE3MGQyYjI0YWQ1ZD"
    "kxOWI5MTY4NTMyOTZmYzM3OGI0ZjI5MWQxMWE0NzEyNDU1YWZkM2QiLCJzaXplX2J5dG"
    "VzIjoxMTUyfQ";

static const char *SIGNATURE_VALID =
    "n5SvkpnQd-pJUI06JWD5hGclrkNCY1eUIIm5ogX2W1fy2keqjIvydPMpOkJiD0U1v6v9"
    "s2-JPfYVQ5k2cjiPBw";
static const char *SIGNATURE_WRONG_KEY =
    "aV1IqTYXBAq6E9C130Je2A0-mCBw9K9zqB-_Q4BNP7SAu9bDrhdzfIvea_ckg65MgsYk"
    "4Potsuzlqat858CNDQ";
static const char *SIGNATURE_TRUNCATED =
    "n5SvkpnQd-pJUI06JWD5hGclrkNCY1eUIIm5ogX2W1fy2keqjIvydPMpOkJiD0U1v6v9"
    "s2-JPf";

static const char *PAYLOAD_DIGEST_MISMATCH =
    "eyJkZXZpY2Vfa2luZCI6ImVzcDMyX2NsaWVudCIsImZ3X3ZlcnNpb24iOiIxLjQuMCIs"
    "ImltYWdlX3VybCI6Imh0dHBzOi8vY2RuLmV4YW1wbGUudGVzdC90ZXNzZXJhZS9lc3Az"
    "Ml9jbGllbnQvYXBwLTEuNC4wLmJpbiIsImtleV9pZCI6InRlc3QtZWQyNTUxOS0xIiwic2"
    "NoZW1hX3ZlcnNpb24iOjEsInNoYTI1NiI6IjAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMD"
    "AwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAiLCJzaXplX2J5"
    "dGVzIjoxMTUyfQ";
static const char *SIGNATURE_DIGEST_MISMATCH =
    "-x7Y5tdUHF71DygGVS3_KL1d0PRPKCmaCaS1F2gtsXE4kfDFfj6OVWJ9sYFBdOweahYHC"
    "c4CSblbNjc2UJXSDQ";

static void test_rfc8032_empty_message(void)
{
    static const uint8_t public_key[32] = {
        0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7,
        0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
        0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25,
        0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a,
    };
    static const uint8_t signature[64] = {
        0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72,
        0x90, 0x86, 0xe2, 0xcc, 0x80, 0x6e, 0x82, 0x8a,
        0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74,
        0xd8, 0x73, 0xe0, 0x65, 0x22, 0x49, 0x01, 0x55,
        0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b, 0xac,
        0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b,
        0xd2, 0x5b, 0xf5, 0xf0, 0x59, 0x5b, 0xbe, 0x24,
        0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b,
    };
    static const uint8_t empty[1] = {0};
    assert(ota_ed25519_verify(public_key, empty, 0, signature));
}

static void test_project_fixtures(void)
{
    ota_verified_payload_t verified;
    assert(ota_verify_signed_payload(PAYLOAD_VALID, SIGNATURE_VALID, &verified)
           == OTA_VERIFY_OK);
    assert(strcmp(verified.verified_key_id, "test-ed25519-1") == 0);
    assert(strstr((char *)verified.bytes, "\"key_id\":\"test-ed25519-1\"") != NULL);

    assert(ota_verify_signed_payload(PAYLOAD_VALID, SIGNATURE_WRONG_KEY, &verified)
           == OTA_VERIFY_BAD_SIGNATURE);
    assert(ota_verify_signed_payload(PAYLOAD_VALID, SIGNATURE_TRUNCATED, &verified)
           == OTA_VERIFY_MALFORMED_DESCRIPTOR);

    /* This descriptor is validly signed; digest_mismatch is a later image gate. */
    assert(ota_verify_signed_payload(PAYLOAD_DIGEST_MISMATCH,
                                     SIGNATURE_DIGEST_MISMATCH, &verified)
           == OTA_VERIFY_OK);
}

static void test_malformed_base64url(void)
{
    ota_verified_payload_t verified;
    assert(ota_verify_signed_payload("abc=", SIGNATURE_VALID, &verified)
           == OTA_VERIFY_MALFORMED_DESCRIPTOR);
    assert(ota_verify_signed_payload("A", SIGNATURE_VALID, &verified)
           == OTA_VERIFY_MALFORMED_DESCRIPTOR);
}

static void test_manifest_checks(void)
{
    ota_manifest_t manifest;
    assert(ota_descriptor_check(PAYLOAD_VALID, SIGNATURE_VALID,
                                "esp32_client", "1.3.0", &manifest)
           == OTA_VERIFY_OK);
    assert(strcmp(manifest.key_id, "test-ed25519-1") == 0);
    assert(strcmp(manifest.fw_version, "1.4.0") == 0);
    assert(manifest.size_bytes == 1152);

    assert(ota_descriptor_check(PAYLOAD_VALID, SIGNATURE_VALID,
                                "pi_png_client", "1.3.0", &manifest)
           == OTA_VERIFY_KIND_MISMATCH);
    assert(ota_descriptor_check(PAYLOAD_VALID, SIGNATURE_VALID,
                                "esp32_client", "1.4.0", &manifest)
           == OTA_VERIFY_ALREADY_CURRENT);

    /* A signed descriptor with an intentionally wrong image digest still
     * passes checks 1-3. The streaming installer owns digest_mismatch. */
    assert(ota_descriptor_check(PAYLOAD_DIGEST_MISMATCH,
                                SIGNATURE_DIGEST_MISMATCH,
                                "esp32_client", "1.3.0", &manifest)
           == OTA_VERIFY_OK);
    for (size_t i = 0; i < sizeof manifest.sha256; i++)
        assert(manifest.sha256[i] == 0);
}

static ota_verify_reason_t parse_authenticated_json(const char *json,
                                                     const char *key_id)
{
    ota_verified_payload_t verified = {0};
    size_t length = strlen(json);
    assert(length <= OTA_MAX_PAYLOAD_BYTES);
    memcpy(verified.bytes, json, length);
    verified.length = length;
    verified.verified_key_id = key_id;
    ota_manifest_t manifest;
    return ota_manifest_parse_and_check(&verified, "esp32_client", "1.3.0",
                                        &manifest);
}

static void test_authenticated_manifest_shape(void)
{
    assert(parse_authenticated_json("{}", "test-ed25519-1")
           == OTA_VERIFY_MALFORMED_MANIFEST);
    assert(parse_authenticated_json("{\"schema_version\":2}",
                                    "test-ed25519-1")
           == OTA_VERIFY_SCHEMA_VERSION);

    static const char *wrong_declared_key =
        "{\"schema_version\":1,\"key_id\":\"prod-1\","
        "\"device_kind\":\"esp32_client\",\"fw_version\":\"2.0.0\","
        "\"image_url\":\"https://example.test/app.bin\",\"size_bytes\":1,"
        "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}";
    assert(parse_authenticated_json(wrong_declared_key, "test-ed25519-1")
           == OTA_VERIFY_BAD_SIGNATURE);

    static const char *trailing_garbage =
        "{\"schema_version\":1,\"key_id\":\"test-ed25519-1\","
        "\"device_kind\":\"esp32_client\",\"fw_version\":\"2.0.0\","
        "\"image_url\":\"https://example.test/app.bin\",\"size_bytes\":1,"
        "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}x";
    assert(parse_authenticated_json(trailing_garbage, "test-ed25519-1")
           == OTA_VERIFY_MALFORMED_MANIFEST);
}

int main(void)
{
    test_rfc8032_empty_message();
    test_project_fixtures();
    test_malformed_base64url();
    test_manifest_checks();
    test_authenticated_manifest_shape();
    puts("ota_verify tests passed");
    return 0;
}
