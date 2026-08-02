/*
 * Host-side tests for relay_crypto.c against the cloud-relay contract's GOLDEN
 * VECTORS (docs/relay/contract.md, "Golden vectors").
 *
 * These are the load-bearing tests for the whole relay feature. Every other
 * failure mode announces itself -- a bad URL 404s, a bad token 401s -- but a
 * derivation that is merely *plausible* yields a wrong key, then a failed GCM
 * tag, and the only symptom is "frames never paint" with nothing to point at.
 * So this pins the exact bytes the contract publishes, using the same
 * Monocypher + mbedTLS code the device runs.
 *
 * Build + run: tools/test_relay_crypto.sh
 */
#include <stdio.h>
#include <string.h>

#include "relay_crypto.h"
#include "relay_wire.h"

static int tests = 0, fails = 0;
#define CHECK(cond) do { tests++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- the contract's vectors, verbatim ---- */

/* Fixed private scalars: home_priv = 0x01 * 32, panel_priv = 0x02 * 32. */
static const char HOME_PUB_HEX[] =
    "a4e09292b651c278b9772c569f5fa9bb13d906b46ab68c9df9dc2b4409f8a209";
static const char PANEL_PUB_HEX[] =
    "ce8d3ad1ccb633ec7b70c17814a5c76ecd029685050d344745ba05870e587d59";
static const char FRAME_KEY_HEX[] =
    "613376ae6bc97931b6d33c17aaf561fb2ff2e2f12937249705e1e5b75dd98e83";
static const char FRAME_HEX[] =
    "74657373657261652d6672616d652d0001027061796c6f6164";
/* The contract prints this across two lines; concatenated verbatim. */
static const char SEALED_HEX[] =
    "00112233445566778899aabb4fa3210211222b8aa47f010d2a30fc71c"
    "70e6de8d4345adf90c057a2b39262b5ab8cee9507d7839ce8";

static size_t unhex(uint8_t *out, size_t cap, const char *hex)
{
    size_t n = 0;
    for (const char *p = hex; p[0] && p[1]; p += 2) {
        unsigned v;
        if (n >= cap || sscanf(p, "%2x", &v) != 1) return 0;
        out[n++] = (uint8_t)v;
    }
    return n;
}

static bool eq_hex(const uint8_t *buf, size_t len, const char *hex)
{
    uint8_t want[128];
    size_t n = unhex(want, sizeof want, hex);
    return n == len && memcmp(buf, want, len) == 0;
}

static void test_public_keys(void)
{
    uint8_t home_priv[32], panel_priv[32], pub[32];
    memset(home_priv, 0x01, sizeof home_priv);
    memset(panel_priv, 0x02, sizeof panel_priv);

    /* X25519 clamps the scalar, so these blunt fixed values are legal keys. */
    relay_public_key(pub, home_priv);
    CHECK(eq_hex(pub, sizeof pub, HOME_PUB_HEX));

    relay_public_key(pub, panel_priv);
    CHECK(eq_hex(pub, sizeof pub, PANEL_PUB_HEX));

    /* keypair_from_seed must agree with public_key for the same scalar. */
    uint8_t priv2[32], pub2[32];
    relay_keypair_from_seed(priv2, pub2, panel_priv);
    CHECK(memcmp(priv2, panel_priv, 32) == 0);
    CHECK(eq_hex(pub2, sizeof pub2, PANEL_PUB_HEX));
}

static void test_derive(void)
{
    uint8_t home_priv[32], panel_priv[32], home_pub[32], panel_pub[32];
    memset(home_priv, 0x01, sizeof home_priv);
    memset(panel_priv, 0x02, sizeof panel_priv);
    relay_public_key(home_pub, home_priv);
    relay_public_key(panel_pub, panel_priv);

    /* The panel's side: our private key + the home public key from pairing. */
    uint8_t key_panel[RELAY_KEY_LEN];
    CHECK(relay_derive_key(key_panel, panel_priv, home_pub));
    CHECK(eq_hex(key_panel, sizeof key_panel, FRAME_KEY_HEX));

    /* The home side derives the identical key from the mirrored inputs -- if
     * this ever diverges the two ends silently stop understanding each other. */
    uint8_t key_home[RELAY_KEY_LEN];
    CHECK(relay_derive_key(key_home, home_priv, panel_pub));
    CHECK(memcmp(key_panel, key_home, RELAY_KEY_LEN) == 0);

    /* A degenerate peer key must be REFUSED, not silently turned into a key
     * the attacker also knows. */
    uint8_t zero_pub[32] = { 0 };
    uint8_t key_bad[RELAY_KEY_LEN];
    CHECK(!relay_derive_key(key_bad, panel_priv, zero_pub));
}

static void test_unseal(void)
{
    uint8_t key[RELAY_KEY_LEN];
    CHECK(unhex(key, sizeof key, FRAME_KEY_HEX) == RELAY_KEY_LEN);

    uint8_t blob[128];
    size_t blob_len = unhex(blob, sizeof blob, SEALED_HEX);
    /* 12 nonce + 25 frame + 16 tag. */
    CHECK(blob_len == 53);

    uint8_t *frame = NULL;
    size_t frame_len = 0;
    CHECK(relay_unseal(blob, blob_len, key, &frame, &frame_len));
    CHECK(frame_len == 25);
    CHECK(frame == blob + RELAY_NONCE_LEN);      /* decrypted IN PLACE */
    CHECK(frame && eq_hex(frame, frame_len, FRAME_HEX));
    CHECK(frame && memcmp(frame, "tesserae-frame-", 15) == 0);

    /* A flipped ciphertext byte must fail the tag, not paint garbage. */
    blob_len = unhex(blob, sizeof blob, SEALED_HEX);
    blob[20] ^= 0x01;
    CHECK(!relay_unseal(blob, blob_len, key, &frame, &frame_len));
    CHECK(frame == NULL && frame_len == 0);

    /* A flipped TAG byte likewise. */
    blob_len = unhex(blob, sizeof blob, SEALED_HEX);
    blob[blob_len - 1] ^= 0x80;
    CHECK(!relay_unseal(blob, blob_len, key, &frame, &frame_len));

    /* Wrong key. */
    blob_len = unhex(blob, sizeof blob, SEALED_HEX);
    uint8_t wrong[RELAY_KEY_LEN];
    memcpy(wrong, key, sizeof wrong);
    wrong[0] ^= 0xFF;
    CHECK(!relay_unseal(blob, blob_len, wrong, &frame, &frame_len));

    /* Truncated blobs: shorter than nonce+tag has no plaintext at all. */
    blob_len = unhex(blob, sizeof blob, SEALED_HEX);
    CHECK(!relay_unseal(blob, RELAY_SEAL_OVERHEAD - 1, key, &frame, &frame_len));
    CHECK(!relay_unseal(blob, 0, key, &frame, &frame_len));
}

/* An empty frame is a legal seal (nonce + tag only) and must not be mistaken
 * for a truncated blob. */
static void test_empty_frame(void)
{
    uint8_t home_priv[32], panel_priv[32], home_pub[32];
    memset(home_priv, 0x01, sizeof home_priv);
    memset(panel_priv, 0x02, sizeof panel_priv);
    relay_public_key(home_pub, home_priv);
    uint8_t key[RELAY_KEY_LEN];
    CHECK(relay_derive_key(key, panel_priv, home_pub));

    /* Sealed empty frame, generated by the reference implementation
     * (app/relay_crypto.py seal(b"", key, nonce=<contract nonce>)) with the
     * contract's own key and nonce -- not hand-written. */
    uint8_t blob[RELAY_SEAL_OVERHEAD];
    size_t n = unhex(blob, sizeof blob,
                     "00112233445566778899aabb"
                     "0b17c8e213d605cf638370e81d0fc59d");
    CHECK(n == RELAY_SEAL_OVERHEAD);
    uint8_t *frame = (uint8_t *)1;
    size_t frame_len = 99;
    bool ok = relay_unseal(blob, n, key, &frame, &frame_len);
    CHECK(ok);
    CHECK(!ok || frame_len == 0);
}

static void test_b64url(void)
{
    /* Round-trip the contract's own public keys, which is the only place the
     * encoding actually appears on the wire. */
    uint8_t pub[32];
    CHECK(unhex(pub, sizeof pub, PANEL_PUB_HEX) == 32);

    char enc[RELAY_B64_KEY_CAP];
    CHECK(relay_b64url_encode(enc, sizeof enc, pub, sizeof pub));
    CHECK(strlen(enc) == 43);                 /* 32 bytes, unpadded */
    CHECK(strchr(enc, '=') == NULL);          /* no padding */
    CHECK(strchr(enc, '+') == NULL && strchr(enc, '/') == NULL);

    uint8_t back[32];
    size_t back_len = 0;
    CHECK(relay_b64url_decode(back, sizeof back, &back_len, enc));
    CHECK(back_len == 32 && memcmp(back, pub, 32) == 0);

    /* Padded input is accepted (servers differ on emitting it). */
    char padded[RELAY_B64_KEY_CAP + 2];
    snprintf(padded, sizeof padded, "%s=", enc);
    CHECK(relay_b64url_decode(back, sizeof back, &back_len, padded));
    CHECK(back_len == 32);

    /* Standard-alphabet input decodes too, rather than failing pairing for a
     * reason no log would explain. */
    uint8_t raw[3] = { 0xFB, 0xFF, 0xBF };
    char std_enc[8];
    CHECK(relay_b64url_encode(std_enc, sizeof std_enc, raw, sizeof raw));
    CHECK(strcmp(std_enc, "-_-_") == 0);
    CHECK(relay_b64url_decode(back, sizeof back, &back_len, "+/+/"));
    CHECK(back_len == 3 && memcmp(back, raw, 3) == 0);

    /* Rejections: bad char, and a buffer too small in each direction. */
    CHECK(!relay_b64url_decode(back, sizeof back, &back_len, "abc$"));
    CHECK(!relay_b64url_decode(back, 2, &back_len, enc));
    char tiny[8];
    CHECK(!relay_b64url_encode(tiny, sizeof tiny, pub, sizeof pub));

    /* Empty encodes to empty and back. */
    CHECK(relay_b64url_encode(enc, sizeof enc, (const uint8_t *)"", 0));
    CHECK(enc[0] == '\0');
    CHECK(relay_b64url_decode(back, sizeof back, &back_len, ""));
    CHECK(back_len == 0);
}


/* ---- pairing response + mailbox URLs ------------------------------------
 * The install_id in a "ready" response is what unblocked the whole feature:
 * without it a paired panel cannot address its own mailbox. These pin the
 * shape the deployed relay actually returns (Worker 7d7c3aa5). */

/* Verbatim from prod, ids shortened. */
static const char READY_JSON[] =
    "{\"status\":\"ready\",\"install_id\":\"inst_abc123\","
    "\"home_pubkey\":\"pOCSkrZRwni5dyxWn1-puxPZBrRqtoyd-dwrRAn4ogk\","
    "\"device_token\":\"tok_secret\",\"device_id\":\"dev_xyz\","
    "\"config\":{}}";

static void test_parse_ready(void)
{
    relay_pairing_t pr;
    CHECK(relay_parse_ready(READY_JSON, strlen(READY_JSON), &pr)
          == RELAY_READY_OK);
    CHECK(strcmp(pr.install_id, "inst_abc123") == 0);
    CHECK(strcmp(pr.device_id, "dev_xyz") == 0);
    CHECK(strcmp(pr.device_token, "tok_secret") == 0);
    /* home_pubkey is the contract's own vector, base64url of HOME_PUB_HEX. */
    CHECK(eq_hex(pr.home_pub, sizeof pr.home_pub, HOME_PUB_HEX));

    /* Pending is a normal state, not an error. */
    const char *pending = "{\"status\":\"pending\"}";
    CHECK(relay_parse_ready(pending, strlen(pending), &pr)
          == RELAY_READY_PENDING);

    /* install_id inside config still works (older / self-hosted Worker). */
    const char *in_cfg =
        "{\"status\":\"ready\",\"home_pubkey\":"
        "\"pOCSkrZRwni5dyxWn1-puxPZBrRqtoyd-dwrRAn4ogk\","
        "\"device_token\":\"t\",\"device_id\":\"d\","
        "\"config\":{\"install_id\":\"inst_from_cfg\"}}";
    CHECK(relay_parse_ready(in_cfg, strlen(in_cfg), &pr) == RELAY_READY_OK);
    CHECK(strcmp(pr.install_id, "inst_from_cfg") == 0);

    /* A "ready" MISSING install_id must be malformed, never half-adopted --
     * this is exactly the state that stranded pairing before the fix. */
    const char *no_inst =
        "{\"status\":\"ready\",\"home_pubkey\":"
        "\"pOCSkrZRwni5dyxWn1-puxPZBrRqtoyd-dwrRAn4ogk\","
        "\"device_token\":\"t\",\"device_id\":\"d\",\"config\":{}}";
    CHECK(relay_parse_ready(no_inst, strlen(no_inst), &pr)
          == RELAY_READY_MALFORMED);
    CHECK(pr.install_id[0] == '\0');       /* and nothing partially written */

    /* Other partial/!well-formed cases. */
    const char *no_key =
        "{\"status\":\"ready\",\"install_id\":\"i\","
        "\"device_token\":\"t\",\"device_id\":\"d\"}";
    CHECK(relay_parse_ready(no_key, strlen(no_key), &pr)
          == RELAY_READY_MALFORMED);
    const char *short_key =
        "{\"status\":\"ready\",\"install_id\":\"i\","
        "\"home_pubkey\":\"AAAA\","
        "\"device_token\":\"t\",\"device_id\":\"d\"}";
    CHECK(relay_parse_ready(short_key, strlen(short_key), &pr)
          == RELAY_READY_MALFORMED);
    CHECK(relay_parse_ready("not json", 8, &pr) == RELAY_READY_MALFORMED);
}

static void test_mailbox_url(void)
{
    char url[320];
    CHECK(relay_mailbox_url(url, sizeof url, "https://relay.tesserae.ink",
                            "inst_abc123", "dev_xyz", "frame"));
    CHECK(strcmp(url,
        "https://relay.tesserae.ink/v1/i/inst_abc123/d/dev_xyz/frame") == 0);

    CHECK(relay_mailbox_url(url, sizeof url, "https://relay.tesserae.ink",
                            "inst_abc123", "dev_xyz", "status"));
    CHECK(strcmp(url,
        "https://relay.tesserae.ink/v1/i/inst_abc123/d/dev_xyz/status") == 0);

    /* Any missing part must refuse rather than build a URL with a hole in it. */
    CHECK(!relay_mailbox_url(url, sizeof url, "", "i", "d", "frame"));
    CHECK(!relay_mailbox_url(url, sizeof url, "https://r", "", "d", "frame"));
    CHECK(!relay_mailbox_url(url, sizeof url, "https://r", "i", "", "frame"));
    /* Truncation must fail closed, not silently request a short URL. */
    char tiny[20];
    CHECK(!relay_mailbox_url(tiny, sizeof tiny, "https://relay.tesserae.ink",
                             "inst_abc123", "dev_xyz", "frame"));
    CHECK(tiny[0] == '\0');
}


/* The optional self-report on POST /v1/pair. Wrong values here are SILENT --
 * a mono panel that claims esp32_client gets a 4bpp Spectra packer -- so pin
 * both the shape and the omit-when-unknown behaviour. */
static void test_pair_body(void)
{
    char body[512];

    /* Full self-report, as a Spectra board sends it. */
    CHECK(relay_build_pair_body(body, sizeof body, "ABC123", "cHVia2V5",
                                800, 480, "esp32_client", "spectra_6"));
    CHECK(strstr(body, "\"code\":\"ABC123\"") != NULL);
    CHECK(strstr(body, "\"panel_pubkey\":\"cHVia2V5\"") != NULL);
    CHECK(strstr(body, "\"panel_w\":800") != NULL);
    CHECK(strstr(body, "\"panel_h\":480") != NULL);
    CHECK(strstr(body, "\"model\":\"esp32_client\"") != NULL);
    CHECK(strstr(body, "\"gamut\":\"spectra_6\"") != NULL);

    /* A mono board reports the OTHER kind -- the case where omitting or
     * defaulting to esp32_client would silently mis-pack every frame. */
    CHECK(relay_build_pair_body(body, sizeof body, "C", "K", 800, 480,
                                "esp32_bw_client", "mono"));
    CHECK(strstr(body, "\"model\":\"esp32_bw_client\"") != NULL);
    CHECK(strstr(body, "\"gamut\":\"mono\"") != NULL);

    /* Bare request is still valid: every extra field is optional, and each is
     * independent of the others. */
    CHECK(relay_build_pair_body(body, sizeof body, "C", "K", 0, 0, NULL, NULL));
    CHECK(strstr(body, "\"code\":\"C\"") != NULL);
    CHECK(strstr(body, "panel_w") == NULL);
    CHECK(strstr(body, "panel_h") == NULL);
    CHECK(strstr(body, "model") == NULL);
    CHECK(strstr(body, "gamut") == NULL);

    CHECK(relay_build_pair_body(body, sizeof body, "C", "K", 1872, 0, "", NULL));
    CHECK(strstr(body, "\"panel_w\":1872") != NULL);
    CHECK(strstr(body, "panel_h") == NULL);
    CHECK(strstr(body, "model") == NULL);   /* empty string omits, not "" */

    /* Required fields really are required, and a body that would not fit fails
     * closed rather than sending a truncated (invalid) JSON document. */
    CHECK(!relay_build_pair_body(body, sizeof body, "", "K", 0, 0, NULL, NULL));
    CHECK(!relay_build_pair_body(body, sizeof body, "C", "", 0, 0, NULL, NULL));
    char tiny[16];
    CHECK(!relay_build_pair_body(tiny, sizeof tiny, "C", "K", 800, 480,
                                 "esp32_client", "spectra_6"));
    CHECK(tiny[0] == '\0');
}

int main(void)
{
    test_public_keys();
    test_derive();
    test_unseal();
    test_empty_frame();
    test_b64url();
    test_parse_ready();
    test_mailbox_url();
    test_pair_body();
    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "ok", tests, fails);
    return fails ? 1 : 0;
}
