#!/bin/sh
# Host-side frame-cache manifest, cache differ and album playback tests.
set -eu

PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
CJSON_DIR="$PIO_CORE_DIR/packages/framework-espidf/components/json/cJSON"
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
    echo "ESP-IDF cJSON source not found at $CJSON_DIR" >&2
    exit 1
fi

OUT="$(mktemp -t tesserae-frame-collection.XXXXXX)"
cc -std=c11 -Wall -Wextra -Werror \
   -I src -I "$CJSON_DIR" \
   test/test_frame_collection.c src/frame_collection.c "$CJSON_DIR/cJSON.c" \
   -lm -o "$OUT"
"$OUT" "$@"
