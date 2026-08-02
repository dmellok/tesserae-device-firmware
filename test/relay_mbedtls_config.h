/*
 * Minimal mbedTLS configuration for the HOST-side relay crypto tests only.
 * Enables exactly the primitives relay_crypto.c uses -- SHA-256, HKDF, and
 * AES-256-GCM -- which keeps the host build down to nine mbedTLS files with no
 * bignum or ECP. X25519 comes from Monocypher, not mbedTLS.
 *
 * Device builds never see this file: there, ESP-IDF supplies its own mbedTLS
 * configuration and the full library. It exists so tools/test_relay_crypto.sh
 * can compile the REAL relay_crypto.c against the contract's golden vectors.
 */
#pragma once

#define MBEDTLS_SHA256_C
#define MBEDTLS_MD_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
