#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ble_setup_protocol.h"

static void test_qr_round_trip_and_replay(void)
{
    uint8_t key[BLE_SETUP_KEY_LEN];
    for (size_t i = 0; i < sizeof key; i++) key[i] = (uint8_t)(i * 7 + 3);
    const uint8_t sid[4] = {0x12, 0x34, 0x56, 0x78};
    const uint8_t connection_nonce[BLE_SETUP_CONN_NONCE_LEN] = {
        0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
        0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,
    };
    ble_setup_crypto_t sender, receiver;
    assert(ble_setup_crypto_init(&sender, key, sid, connection_nonce));
    assert(ble_setup_crypto_init(&receiver, key, sid, connection_nonce));

    const uint8_t message[] = "{\"op\":\"scan\"}";
    uint8_t sealed[128], opened[128];
    size_t sealed_len = 0, opened_len = 0;
    assert(ble_setup_seal(&sender, BLE_SETUP_DIR_APP_TO_DEVICE,
                          message, sizeof message - 1,
                          sealed, sizeof sealed, &sealed_len));
    const uint8_t expected[] = {
        0xa1,0x00,0x00,0x00,0x01,0x7a,0xa1,0x63,0x18,0x46,0xfa,
        0x0c,0x8f,0x2a,0x24,0xb8,0x39,0x01,0xd9,0xb1,0x1e,0x27,
        0x15,0xbd,0x50,0x5e,0xa2,0x4f,0xa8,0x60,0x62,0x43,0x2f,
        0x11,
    };
    assert(sealed_len == sizeof expected);
    assert(memcmp(sealed, expected, sizeof expected) == 0);
    assert(ble_setup_open(&receiver, BLE_SETUP_DIR_APP_TO_DEVICE,
                          sealed, sealed_len, opened, sizeof opened, &opened_len));
    assert(opened_len == sizeof message - 1);
    assert(memcmp(opened, message, opened_len) == 0);
    assert(!ble_setup_open(&receiver, BLE_SETUP_DIR_APP_TO_DEVICE,
                           sealed, sealed_len, opened, sizeof opened, &opened_len));

    sealed[sealed_len - 1] ^= 1;
    assert(ble_setup_crypto_init(&receiver, key, sid, connection_nonce));
    assert(!ble_setup_open(&receiver, BLE_SETUP_DIR_APP_TO_DEVICE,
                           sealed, sealed_len, opened, sizeof opened, &opened_len));
}

static void test_connection_nonce_blocks_cross_connection_replay(void)
{
    uint8_t secret[BLE_SETUP_KEY_LEN];
    for (size_t i = 0; i < sizeof secret; i++) secret[i] = (uint8_t)i;
    const uint8_t sid[BLE_SETUP_SID_LEN] = {1, 2, 3, 4};
    uint8_t first_nonce[BLE_SETUP_CONN_NONCE_LEN] = {0};
    uint8_t second_nonce[BLE_SETUP_CONN_NONCE_LEN] = {0};
    second_nonce[BLE_SETUP_CONN_NONCE_LEN - 1] = 1;
    ble_setup_crypto_t first, second;
    assert(ble_setup_crypto_init(&first, secret, sid, first_nonce));
    assert(ble_setup_crypto_init(&second, secret, sid, second_nonce));

    const uint8_t message[] = "reconnect";
    uint8_t frame[64], opened[64];
    size_t frame_len = 0, opened_len = 0;
    assert(ble_setup_seal(&first, BLE_SETUP_DIR_APP_TO_DEVICE,
                          message, sizeof message - 1,
                          frame, sizeof frame, &frame_len));
    assert(!ble_setup_open(&second, BLE_SETUP_DIR_APP_TO_DEVICE,
                           frame, frame_len, opened, sizeof opened, &opened_len));

    ble_setup_crypto_t second_sender;
    assert(ble_setup_crypto_init(&second_sender, secret, sid, second_nonce));
    assert(ble_setup_seal(&second_sender, BLE_SETUP_DIR_APP_TO_DEVICE,
                          message, sizeof message - 1,
                          frame, sizeof frame, &frame_len));
    assert(ble_setup_open(&second, BLE_SETUP_DIR_APP_TO_DEVICE,
                          frame, frame_len, opened, sizeof opened, &opened_len));
}

static void test_chunk_reassembly(void)
{
    const char *parts[] = {"{\"op\":", "\"stage\",", "\"ssid\":\"Home\"}"};
    ble_setup_reassembly_t state;
    ble_setup_reassembly_reset(&state);
    for (uint8_t i = 0; i < 3; i++) {
        uint8_t raw[64]; size_t raw_len = 0;
        assert(ble_setup_chunk_encode(42, i, 3,
                                      (const uint8_t *)parts[i], strlen(parts[i]),
                                      raw, sizeof raw, &raw_len));
        ble_setup_chunk_t chunk;
        assert(ble_setup_chunk_decode(raw, raw_len, &chunk));
        bool valid = false;
        bool done = ble_setup_reassembly_push(&state, &chunk, &valid);
        assert(valid);
        assert(done == (i == 2));
    }
    assert(strcmp((const char *)state.bytes,
                  "{\"op\":\"stage\",\"ssid\":\"Home\"}") == 0);

    uint8_t raw[16]; size_t raw_len = 0;
    assert(ble_setup_chunk_encode(7, 1, 2, (const uint8_t *)"bad", 3,
                                  raw, sizeof raw, &raw_len));
    ble_setup_chunk_t chunk;
    assert(ble_setup_chunk_decode(raw, raw_len, &chunk));
    bool valid = true;
    assert(!ble_setup_reassembly_push(&state, &chunk, &valid));
    assert(!valid);
}

int main(void)
{
    test_qr_round_trip_and_replay();
    test_connection_nonce_blocks_cross_connection_replay();
    test_chunk_reassembly();
    puts("ble setup protocol tests: PASS");
    return 0;
}
