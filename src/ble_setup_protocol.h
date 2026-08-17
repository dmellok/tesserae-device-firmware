/* BLE setup framing and QR-session crypto. Pure code, host-tested. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLE_SETUP_PROTOCOL_MAJOR 2
#define BLE_SETUP_KEY_LEN       32
#define BLE_SETUP_SID_LEN        4
#define BLE_SETUP_CONN_NONCE_LEN 16
#define BLE_SETUP_TAG_LEN       16
#define BLE_SETUP_CHUNK_HEADER    4
#define BLE_SETUP_MESSAGE_MAX   512

#define BLE_SETUP_FRAME_NATIVE  0xA0
#define BLE_SETUP_FRAME_QR      0xA1

typedef enum {
    BLE_SETUP_DIR_APP_TO_DEVICE = 0,
    BLE_SETUP_DIR_DEVICE_TO_APP = 1,
} ble_setup_direction_t;

typedef struct {
    uint8_t key[BLE_SETUP_KEY_LEN];
    uint8_t sid[BLE_SETUP_SID_LEN];
    uint32_t rx_counter;
    uint32_t tx_counter;
} ble_setup_crypto_t;

typedef struct {
    uint16_t message_id;
    uint8_t chunk_index;
    uint8_t chunk_count;
    const uint8_t *payload;
    size_t payload_len;
} ble_setup_chunk_t;

typedef struct {
    uint16_t message_id;
    uint8_t next_index;
    uint8_t chunk_count;
    size_t len;
    uint8_t bytes[BLE_SETUP_MESSAGE_MAX + 1];
} ble_setup_reassembly_t;

/* Derive a connection-specific frame key from the QR master secret. The
 * connection nonce is regenerated whenever a phone reconnects, allowing the
 * same on-screen QR code to be reused without making captured frames valid in
 * a later connection. */
bool ble_setup_crypto_init(
    ble_setup_crypto_t *ctx,
    const uint8_t secret[BLE_SETUP_KEY_LEN],
    const uint8_t sid[BLE_SETUP_SID_LEN],
    const uint8_t connection_nonce[BLE_SETUP_CONN_NONCE_LEN]);

/* QR frame: A1 || counter_be32 || AES-GCM(chunk, nonce, aad=header) || tag. */
bool ble_setup_seal(ble_setup_crypto_t *ctx, ble_setup_direction_t direction,
                    const uint8_t *plain, size_t plain_len,
                    uint8_t *out, size_t out_cap, size_t *out_len);

bool ble_setup_open(ble_setup_crypto_t *ctx, ble_setup_direction_t direction,
                    const uint8_t *frame, size_t frame_len,
                    uint8_t *plain, size_t plain_cap, size_t *plain_len);

bool ble_setup_chunk_encode(uint16_t message_id, uint8_t chunk_index,
                            uint8_t chunk_count, const uint8_t *payload,
                            size_t payload_len, uint8_t *out, size_t out_cap,
                            size_t *out_len);
bool ble_setup_chunk_decode(const uint8_t *plain, size_t plain_len,
                            ble_setup_chunk_t *out);

void ble_setup_reassembly_reset(ble_setup_reassembly_t *state);

/* Returns true only when a complete message is now available in state->bytes.
 * Invalid/out-of-order input resets the state and reports false through valid. */
bool ble_setup_reassembly_push(ble_setup_reassembly_t *state,
                               const ble_setup_chunk_t *chunk, bool *valid);
