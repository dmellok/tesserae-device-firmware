/* relay_crypto.c -- cloud-relay frame crypto. See relay_crypto.h. */

#include "relay_crypto.h"

#include <string.h>

#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"

#ifdef ESP_PLATFORM
#include "esp_random.h"
#endif

/* Monocypher supplies X25519. It is already vendored for Ed25519 OTA
 * verification, is a single translation unit, and builds unchanged on the
 * host -- which is what lets the golden-vector test cover the real code. */
#include <monocypher.h>

/* HKDF domain separation, from the contract. Changing this string changes the
 * derived key, so both ends must move together -- hence the explicit v1. */
static const char HKDF_INFO[] = "tesserae-relay-frame-key-v1";

void relay_public_key(uint8_t pub[RELAY_PUB_LEN],
                      const uint8_t priv[RELAY_PRIV_LEN])
{
    crypto_x25519_public_key(pub, priv);
}

void relay_keypair_from_seed(uint8_t priv[RELAY_PRIV_LEN],
                             uint8_t pub[RELAY_PUB_LEN],
                             const uint8_t seed[RELAY_PRIV_LEN])
{
    memcpy(priv, seed, RELAY_PRIV_LEN);
    crypto_x25519_public_key(pub, priv);
}

#ifdef ESP_PLATFORM
void relay_keypair(uint8_t priv[RELAY_PRIV_LEN], uint8_t pub[RELAY_PUB_LEN])
{
    uint8_t seed[RELAY_PRIV_LEN];
    esp_fill_random(seed, sizeof seed);      /* hardware RNG */
    relay_keypair_from_seed(priv, pub, seed);
    crypto_wipe(seed, sizeof seed);
}
#endif

bool relay_derive_key(uint8_t key[RELAY_KEY_LEN],
                      const uint8_t our_priv[RELAY_PRIV_LEN],
                      const uint8_t their_pub[RELAY_PUB_LEN])
{
    uint8_t shared[32];
    crypto_x25519(shared, our_priv, their_pub);

    /* An all-zero shared secret means the peer key was in a small subgroup
     * (or simply zero). Monocypher reports this by zeroing the output rather
     * than failing, so check explicitly: continuing would derive a key an
     * attacker also knows. Constant-time compare -- crypto_verify32 returns 0
     * when the buffers are equal. */
    static const uint8_t zero[32] = { 0 };
    if (crypto_verify32(shared, zero) == 0) {
        crypto_wipe(shared, sizeof shared);
        return false;
    }

    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL) {
        crypto_wipe(shared, sizeof shared);
        return false;
    }
    /* salt = NULL/0 is RFC 5869's "no salt", i.e. HashLen zero bytes -- the
     * same thing Python's cryptography does for salt=None. */
    int rc = mbedtls_hkdf(md, NULL, 0, shared, sizeof shared,
                          (const unsigned char *)HKDF_INFO,
                          sizeof HKDF_INFO - 1, key, RELAY_KEY_LEN);
    crypto_wipe(shared, sizeof shared);
    if (rc != 0) {
        memset(key, 0, RELAY_KEY_LEN);
        return false;
    }
    return true;
}

bool relay_unseal(uint8_t *blob, size_t blob_len,
                  const uint8_t key[RELAY_KEY_LEN],
                  uint8_t **frame, size_t *frame_len)
{
    if (frame) *frame = NULL;
    if (frame_len) *frame_len = 0;
    if (blob == NULL || key == NULL) return false;
    /* Must hold at least a nonce and a tag; a zero-length frame is legal. */
    if (blob_len < RELAY_SEAL_OVERHEAD) return false;

    const uint8_t *nonce = blob;
    uint8_t *ct = blob + RELAY_NONCE_LEN;
    size_t ct_len = blob_len - RELAY_SEAL_OVERHEAD;
    const uint8_t *tag = ct + ct_len;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key,
                                RELAY_KEY_LEN * 8);
    if (rc == 0) {
        /* output == input: decrypt in place, so a 1.3 MB frame needs no second
         * buffer. mbedTLS verifies the tag before returning success. */
        rc = mbedtls_gcm_auth_decrypt(&gcm, ct_len, nonce, RELAY_NONCE_LEN,
                                      NULL, 0,           /* aad = "" */
                                      tag, RELAY_TAG_LEN,
                                      ct, ct);
    }
    mbedtls_gcm_free(&gcm);
    if (rc != 0) return false;

    if (frame) *frame = ct;
    if (frame_len) *frame_len = ct_len;
    return true;
}

/* ---- base64url ----------------------------------------------------------- */

static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

bool relay_b64url_encode(char *out, size_t out_cap,
                         const uint8_t *in, size_t len)
{
    if (out == NULL || (in == NULL && len)) return false;
    size_t need = (len + 2) / 3 * 4;         /* unpadded still rounds groups */
    if (len % 3 == 1)      need -= 2;
    else if (len % 3 == 2) need -= 1;
    if (out_cap < need + 1) return false;

    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        size_t rem = len - i;
        if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
        if (rem > 2) v |= in[i + 2];
        out[o++] = B64URL[(v >> 18) & 0x3F];
        out[o++] = B64URL[(v >> 12) & 0x3F];
        if (rem > 1) out[o++] = B64URL[(v >> 6) & 0x3F];
        if (rem > 2) out[o++] = B64URL[v & 0x3F];
    }
    out[o] = '\0';
    return true;
}

static int b64url_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    /* Accept the standard alphabet too: some encoders emit +/ even for a field
     * documented as base64url, and being strict there would fail pairing for a
     * reason no log would make obvious. */
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool relay_b64url_decode(uint8_t *out, size_t out_cap, size_t *out_len,
                         const char *in)
{
    if (out_len) *out_len = 0;
    if (out == NULL || in == NULL) return false;

    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=') break;                /* padding: tolerated, ends input */
        int v = b64url_val(*p);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) return false;
            out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    /* Leftover bits must be zero padding, never a truncated byte. */
    if (bits >= 6) return false;
    if (bits && (acc & ((1u << bits) - 1)) != 0) return false;

    if (out_len) *out_len = o;
    return true;
}
