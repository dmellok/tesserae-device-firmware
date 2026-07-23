/*
 * deck_digest.c -- device-side SHA-256 backend for deck.c, via mbedTLS.
 *
 * Deliberately a separate translation unit: deck.c stays pure/host-testable,
 * and the host test (test/test_deck.c) links its own reference SHA-256
 * instead of this file. Monocypher (the OTA signature dep) has no SHA-256,
 * so the deck digest contract is pinned to mbedTLS here.
 */
#include "deck.h"

#include "mbedtls/sha256.h"

void deck_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    /* 0 = SHA-256 (not SHA-224). The one-shot helper handles init/free. */
    mbedtls_sha256(data, len, out, 0);
}
