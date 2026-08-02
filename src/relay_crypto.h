/*
 * relay_crypto.h: end-to-end frame crypto for the Tesserae cloud relay.
 *
 * The relay is a store-and-forward mailbox that must never be able to read a
 * dashboard. At pairing the panel and the home instance exchange X25519 PUBLIC
 * keys through the relay and each derive the same 32-byte frame key locally;
 * the key itself is never transmitted. Every frame then arrives as
 *
 *     sealed = nonce(12) || AES-256-GCM(frame, frame_key, nonce, aad="")
 *
 * so the relay holds ciphertext only. Wire contract: docs/relay/contract.md in
 * the Tesserae repo -- that document is authoritative for all three sides
 * (home, Worker, firmware), and its "Golden vectors" section pins these bytes.
 *
 * This file is PURE: X25519 comes from Monocypher (already vendored for Ed25519
 * OTA verification) and HKDF-SHA256 / AES-GCM from mbedTLS, both of which build
 * on the host as well as on device. test/test_relay_crypto.c therefore checks
 * the REAL implementation against the contract's vectors rather than a stand-in
 * -- which matters, because a derivation that is merely plausible produces a
 * silently wrong key and then garbage frames, with nothing to point at.
 *
 * Device orchestration (pairing state machine, polling, NVS) lives in relay.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RELAY_PRIV_LEN   32
#define RELAY_PUB_LEN    32
#define RELAY_KEY_LEN    32
#define RELAY_NONCE_LEN  12
#define RELAY_TAG_LEN    16

/* A sealed blob costs a nonce up front and a GCM tag at the end. */
#define RELAY_SEAL_OVERHEAD (RELAY_NONCE_LEN + RELAY_TAG_LEN)

/* base64url of a 32-byte key, unpadded, plus a NUL. */
#define RELAY_B64_KEY_CAP 44

/* ---- keys ---------------------------------------------------------------- */

/* Derive the public key for a raw 32-byte private scalar. X25519 clamps the
 * scalar internally, so any 32 bytes are a usable private key -- which is why
 * the contract's vectors can use scalars as blunt as 0x01 repeated. */
void relay_public_key(uint8_t pub[RELAY_PUB_LEN],
                      const uint8_t priv[RELAY_PRIV_LEN]);

/* Keypair from caller-supplied entropy. Split out from relay_keypair() so the
 * host tests can pin the contract's fixed scalars; on device the seed comes
 * from the hardware RNG. */
void relay_keypair_from_seed(uint8_t priv[RELAY_PRIV_LEN],
                             uint8_t pub[RELAY_PUB_LEN],
                             const uint8_t seed[RELAY_PRIV_LEN]);

/* Fresh keypair from the platform RNG. Device-only (needs esp_fill_random). */
#ifdef ESP_PLATFORM
void relay_keypair(uint8_t priv[RELAY_PRIV_LEN], uint8_t pub[RELAY_PUB_LEN]);
#endif

/* frame_key = HKDF-SHA256(ikm = X25519(our_priv, their_pub), salt = none,
 *                         info = "tesserae-relay-frame-key-v1", L = 32)
 *
 * Symmetric by construction: derive(home_priv, panel_pub) ==
 * derive(panel_priv, home_pub). "salt = none" means RFC 5869's zero-filled
 * salt of HashLen bytes, which is what both mbedTLS (salt = NULL) and Python's
 * cryptography (salt=None) do -- they agree, and the golden vector proves it.
 *
 * False if the ECDH result is degenerate (an all-zero shared secret, i.e. a
 * peer public key in a small subgroup); the caller must NOT fall back to an
 * unauthenticated frame in that case. */
bool relay_derive_key(uint8_t key[RELAY_KEY_LEN],
                      const uint8_t our_priv[RELAY_PRIV_LEN],
                      const uint8_t their_pub[RELAY_PUB_LEN]);

/* ---- frames -------------------------------------------------------------- */

/* Decrypt a sealed frame IN PLACE.
 *
 * In place because a frame is ~1.3 MB on the larger panels and a second buffer
 * would double the peak: the plaintext simply starts RELAY_NONCE_LEN into the
 * blob, so *frame points at blob + 12 and can be handed straight to the panel
 * driver without a copy.
 *
 * Returns false on a short blob or a failed authentication tag -- which is the
 * only thing standing between a corrupted/hostile mailbox and the panel, so a
 * false here must never be painted. blob is left undefined on failure. */
bool relay_unseal(uint8_t *blob, size_t blob_len,
                  const uint8_t key[RELAY_KEY_LEN],
                  uint8_t **frame, size_t *frame_len);

/* ---- base64url (unpadded), as used for keys on the wire ------------------ */

/* Encode len bytes into out (NUL-terminated). Returns false if out is too
 * small; needs 4*ceil(len/3) + 1. */
bool relay_b64url_encode(char *out, size_t out_cap,
                         const uint8_t *in, size_t len);

/* Decode an unpadded (or padded -- both accepted, servers differ) base64url
 * string. *out_len is the decoded length. False on a bad character or
 * overflow. */
bool relay_b64url_decode(uint8_t *out, size_t out_cap, size_t *out_len,
                         const char *in);
