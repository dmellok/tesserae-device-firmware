#!/bin/sh
set -eu

PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
MBEDTLS="$PIO_CORE_DIR/packages/framework-espidf/components/mbedtls/mbedtls"
if [ ! -d "$MBEDTLS/library" ]; then
    echo "ESP-IDF mbedTLS not found at $MBEDTLS" >&2
    exit 1
fi

OUT="$(mktemp -t tesserae-ble-setup.XXXXXX)"
cc -std=c11 -Wall -Wextra -Werror \
   -DMBEDTLS_CONFIG_FILE='"relay_mbedtls_config.h"' \
   -I src -I test -I "$MBEDTLS/include" -I "$MBEDTLS/library" \
   test/test_ble_setup_protocol.c src/ble_setup_protocol.c \
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
