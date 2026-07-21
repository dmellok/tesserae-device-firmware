#!/bin/sh
set -eu

PIO_BIN="${PIO_BIN:-$HOME/.platformio/penv/bin/pio}"
"$PIO_BIN" pkg install -e seeed-reterminal-e1004 >/dev/null

MONOCYPHER_DIR=".pio/libdeps/seeed-reterminal-e1004/Monocypher"
if [ ! -f "$MONOCYPHER_DIR/src/monocypher.c" ]; then
    echo "Monocypher dependency not found at $MONOCYPHER_DIR" >&2
    exit 1
fi

PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
CJSON_DIR="$PIO_CORE_DIR/packages/framework-espidf/components/json/cJSON"
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
    echo "ESP-IDF cJSON source not found at $CJSON_DIR" >&2
    exit 1
fi

OUT="$(mktemp -t tesserae-ota-verify.XXXXXX)"
cc -std=c11 -Wall -Wextra -Werror \
   -DTESSERAE_OTA_TRUST_TEST_KEY=1 \
   -I src \
   -I "$MONOCYPHER_DIR/src" \
   -I "$MONOCYPHER_DIR/src/optional" \
   -I "$CJSON_DIR" \
   test/test_ota_verify.c \
   src/ota_verify.c \
   src/ota_manifest.c \
   "$CJSON_DIR/cJSON.c" \
   "$MONOCYPHER_DIR/src/monocypher.c" \
   "$MONOCYPHER_DIR/src/optional/monocypher-ed25519.c" \
   -lm \
   -o "$OUT"
"$OUT"
