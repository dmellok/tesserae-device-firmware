#!/bin/sh
# Host-side cloud-relay crypto tests: X25519 + HKDF-SHA256 derivation and
# AES-256-GCM unsealing, checked against the GOLDEN VECTORS in the Tesserae
# repo's docs/relay/contract.md.
#
# This compiles the REAL device code -- Monocypher for X25519 (already vendored
# for Ed25519 OTA verification) and a minimal mbedTLS subset from ESP-IDF for
# HKDF/AES-GCM -- rather than a host stand-in, because the thing worth proving
# is that *our* derivation reproduces the contract byte for byte. A wrong key
# fails silently: pairing "succeeds", every frame then fails its GCM tag, and
# the panel just never updates.
#
# mbedTLS needs a config; mbedtls_config.h next door enables only what the four
# primitives require, so the subset stays at nine files.
set -eu

PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
MBEDTLS="$PIO_CORE_DIR/packages/framework-espidf/components/mbedtls/mbedtls"
if [ ! -d "$MBEDTLS/library" ]; then
    echo "ESP-IDF mbedTLS not found at $MBEDTLS" >&2
    exit 1
fi

# Monocypher arrives as a PlatformIO lib dependency. Fetch it the same way
# tools/test_ota_verify.sh does, so this runs on a clean checkout (CI never
# builds a board in the host-test job) rather than only after a board build.
PIO_BIN="${PIO_BIN:-$HOME/.platformio/penv/bin/pio}"
"$PIO_BIN" pkg install -e seeed-reterminal-e1004 >/dev/null
MONO=".pio/libdeps/seeed-reterminal-e1004/Monocypher/src"
if [ ! -f "$MONO/monocypher.c" ]; then
    echo "Monocypher dependency not found at $MONO" >&2
    exit 1
fi

CJSON_DIR="$PIO_CORE_DIR/packages/framework-espidf/components/json/cJSON"
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
    echo "ESP-IDF cJSON source not found at $CJSON_DIR" >&2
    exit 1
fi

OUT="$(mktemp -t tesserae-relay.XXXXXX)"
cc -std=c11 -Wall -Wextra -Werror \
   -DMBEDTLS_CONFIG_FILE='"relay_mbedtls_config.h"' \
   -I src \
   -I test \
   -I "$MONO" \
   -I "$MBEDTLS/include" \
   -I "$MBEDTLS/library" \
   -I "$CJSON_DIR" \
   test/test_relay_crypto.c \
   src/relay_crypto.c \
   src/relay_wire.c \
   "$CJSON_DIR/cJSON.c" \
   "$MONO/monocypher.c" \
   "$MBEDTLS/library/hkdf.c" \
   "$MBEDTLS/library/md.c" \
   "$MBEDTLS/library/sha256.c" \
   "$MBEDTLS/library/aes.c" \
   "$MBEDTLS/library/gcm.c" \
   "$MBEDTLS/library/cipher.c" \
   "$MBEDTLS/library/cipher_wrap.c" \
   "$MBEDTLS/library/platform_util.c" \
   "$MBEDTLS/library/constant_time.c" \
   -o "$OUT"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
