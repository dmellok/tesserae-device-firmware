#!/bin/sh
# Host-side deck-cache logic tests (manifest parse, hit-testing, digest
# verify, sync differ). Mirrors tools/test_ota_verify.sh: compiles deck.c
# against the ESP-IDF cJSON sources with the system compiler. The SHA-256
# backend comes from the test file itself (reference implementation);
# the device links src/deck_digest.c (mbedTLS) instead.
set -eu

PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
CJSON_DIR="$PIO_CORE_DIR/packages/framework-espidf/components/json/cJSON"
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
    echo "ESP-IDF cJSON source not found at $CJSON_DIR" >&2
    exit 1
fi

OUT="$(mktemp -t tesserae-deck.XXXXXX)"
cc -std=c11 -Wall -Wextra -Werror \
   -I src \
   -I "$CJSON_DIR" \
   test/test_deck.c \
   src/deck.c \
   "$CJSON_DIR/cJSON.c" \
   -lm \
   -o "$OUT"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
