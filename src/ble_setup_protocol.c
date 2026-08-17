#include "ble_setup_protocol.h"

#include <string.h>

#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"

static const char HKDF_INFO[] = "tesserae-ble-connection-key-v2";

static uint32_t read_be32(const uint8_t in[4])
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | in[3];
}

static void write_be32(uint8_t out[4], uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void nonce_for(const ble_setup_crypto_t *ctx,
                      ble_setup_direction_t direction, uint32_t counter,
                      uint8_t nonce[12])
{
    memcpy(nonce, ctx->sid, BLE_SETUP_SID_LEN);
    nonce[4] = (uint8_t)direction;
    nonce[5] = nonce[6] = nonce[7] = 0;
    write_be32(nonce + 8, counter);
}

bool ble_setup_crypto_init(
    ble_setup_crypto_t *ctx,
    const uint8_t secret[BLE_SETUP_KEY_LEN],
    const uint8_t sid[BLE_SETUP_SID_LEN],
    const uint8_t connection_nonce[BLE_SETUP_CONN_NONCE_LEN])
{
    if (!ctx || !secret || !sid || !connection_nonce) return false;
    memset(ctx, 0, sizeof *ctx);
    memcpy(ctx->sid, sid, BLE_SETUP_SID_LEN);
    uint8_t salt[BLE_SETUP_SID_LEN + BLE_SETUP_CONN_NONCE_LEN];
    memcpy(salt, sid, BLE_SETUP_SID_LEN);
    memcpy(salt + BLE_SETUP_SID_LEN, connection_nonce,
           BLE_SETUP_CONN_NONCE_LEN);
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = md == NULL ? -1 : mbedtls_hkdf(
        md, salt, sizeof salt, secret, BLE_SETUP_KEY_LEN,
        (const unsigned char *)HKDF_INFO, sizeof HKDF_INFO - 1,
        ctx->key, BLE_SETUP_KEY_LEN);
    memset(salt, 0, sizeof salt);
    if (rc != 0) {
        memset(ctx, 0, sizeof *ctx);
        return false;
    }
    return true;
}

bool ble_setup_seal(ble_setup_crypto_t *ctx, ble_setup_direction_t direction,
                    const uint8_t *plain, size_t plain_len,
                    uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!ctx || (!plain && plain_len) || !out) return false;
    if (plain_len > BLE_SETUP_MESSAGE_MAX ||
        out_cap < 5 + plain_len + BLE_SETUP_TAG_LEN) return false;

    uint32_t counter = ++ctx->tx_counter;
    out[0] = BLE_SETUP_FRAME_QR;
    write_be32(out + 1, counter);
    uint8_t nonce[12];
    nonce_for(ctx, direction, counter, nonce);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, ctx->key,
                                BLE_SETUP_KEY_LEN * 8);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(
            &gcm, MBEDTLS_GCM_ENCRYPT, plain_len,
            nonce, sizeof nonce, out, 5,
            plain, out + 5, BLE_SETUP_TAG_LEN, out + 5 + plain_len);
    }
    mbedtls_gcm_free(&gcm);
    if (rc != 0) return false;
    if (out_len) *out_len = 5 + plain_len + BLE_SETUP_TAG_LEN;
    return true;
}

bool ble_setup_open(ble_setup_crypto_t *ctx, ble_setup_direction_t direction,
                    const uint8_t *frame, size_t frame_len,
                    uint8_t *plain, size_t plain_cap, size_t *plain_len)
{
    if (plain_len) *plain_len = 0;
    if (!ctx || !frame || !plain || frame_len < 5 + BLE_SETUP_TAG_LEN ||
        frame[0] != BLE_SETUP_FRAME_QR) return false;
    size_t cipher_len = frame_len - 5 - BLE_SETUP_TAG_LEN;
    if (cipher_len > plain_cap || cipher_len > BLE_SETUP_MESSAGE_MAX) return false;

    uint32_t counter = read_be32(frame + 1);
    if (counter == 0 || counter <= ctx->rx_counter) return false;
    uint8_t nonce[12];
    nonce_for(ctx, direction, counter, nonce);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, ctx->key,
                                BLE_SETUP_KEY_LEN * 8);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(
            &gcm, cipher_len, nonce, sizeof nonce, frame, 5,
            frame + 5 + cipher_len, BLE_SETUP_TAG_LEN, frame + 5, plain);
    }
    mbedtls_gcm_free(&gcm);
    if (rc != 0) return false;
    ctx->rx_counter = counter;
    if (plain_len) *plain_len = cipher_len;
    return true;
}

bool ble_setup_chunk_encode(uint16_t message_id, uint8_t chunk_index,
                            uint8_t chunk_count, const uint8_t *payload,
                            size_t payload_len, uint8_t *out, size_t out_cap,
                            size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!out || (!payload && payload_len) || chunk_count == 0 ||
        chunk_index >= chunk_count || out_cap < BLE_SETUP_CHUNK_HEADER + payload_len)
        return false;
    out[0] = (uint8_t)(message_id >> 8);
    out[1] = (uint8_t)message_id;
    out[2] = chunk_index;
    out[3] = chunk_count;
    if (payload_len) memcpy(out + BLE_SETUP_CHUNK_HEADER, payload, payload_len);
    if (out_len) *out_len = BLE_SETUP_CHUNK_HEADER + payload_len;
    return true;
}

bool ble_setup_chunk_decode(const uint8_t *plain, size_t plain_len,
                            ble_setup_chunk_t *out)
{
    if (!plain || !out || plain_len < BLE_SETUP_CHUNK_HEADER || plain[3] == 0 ||
        plain[2] >= plain[3]) return false;
    out->message_id = (uint16_t)((uint16_t)plain[0] << 8 | plain[1]);
    out->chunk_index = plain[2];
    out->chunk_count = plain[3];
    out->payload = plain + BLE_SETUP_CHUNK_HEADER;
    out->payload_len = plain_len - BLE_SETUP_CHUNK_HEADER;
    return true;
}

void ble_setup_reassembly_reset(ble_setup_reassembly_t *state)
{
    if (state) memset(state, 0, sizeof *state);
}

bool ble_setup_reassembly_push(ble_setup_reassembly_t *state,
                               const ble_setup_chunk_t *chunk, bool *valid)
{
    if (valid) *valid = false;
    if (!state || !chunk || chunk->chunk_count == 0 ||
        chunk->chunk_index >= chunk->chunk_count) return false;

    if (chunk->chunk_index == 0) {
        ble_setup_reassembly_reset(state);
        state->message_id = chunk->message_id;
        state->chunk_count = chunk->chunk_count;
    }
    if (state->chunk_count == 0 || state->message_id != chunk->message_id ||
        state->chunk_count != chunk->chunk_count ||
        state->next_index != chunk->chunk_index ||
        state->len + chunk->payload_len > BLE_SETUP_MESSAGE_MAX) {
        ble_setup_reassembly_reset(state);
        return false;
    }
    if (chunk->payload_len) {
        memcpy(state->bytes + state->len, chunk->payload, chunk->payload_len);
        state->len += chunk->payload_len;
    }
    state->next_index++;
    if (valid) *valid = true;
    if (state->next_index == state->chunk_count) {
        state->bytes[state->len] = '\0';
        return true;
    }
    return false;
}
