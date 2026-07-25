#!/bin/sh
# Host-side proto2 logic tests (manifest parse, gestures, hit-test, text blit).
# staleness, hygiene, invert/blit). Mirrors tools/test_deck.sh.
set -eu

PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
CJSON_DIR="$PIO_CORE_DIR/packages/framework-espidf/components/json/cJSON"
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
    echo "ESP-IDF cJSON source not found at $CJSON_DIR" >&2
    exit 1
fi

OUT="$(mktemp -t tesserae-overlay.XXXXXX)"
cc -std=c11 -Wall -Wextra -Werror \
   -I src \
   -I "$CJSON_DIR" \
   test/test_proto2.c \
   src/proto2.c \
   "$CJSON_DIR/cJSON.c" \
   -lm \
   -o "$OUT"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
