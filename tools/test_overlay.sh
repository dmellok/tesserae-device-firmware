#!/bin/sh
# Host-side overlay logic tests (spec parse, glyph fallback, seq rules,
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
   test/test_overlay.c \
   src/overlay.c \
   "$CJSON_DIR/cJSON.c" \
   -lm \
   -o "$OUT"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
