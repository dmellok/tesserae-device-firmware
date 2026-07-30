#!/bin/sh
# Host-side touch-v3 logic tests (spec parse, hit-test, snap/slider math,
# value formatting, vector draw ops + atlas blit on a synthetic 4bpp
# framebuffer). Mirrors tools/test_proto2.sh.
set -eu

PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
CJSON_DIR="$PIO_CORE_DIR/packages/framework-espidf/components/json/cJSON"
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
    echo "ESP-IDF cJSON source not found at $CJSON_DIR" >&2
    exit 1
fi

OUT="$(mktemp -t tesserae-touch3.XXXXXX)"
cc -std=c11 -Wall -Wextra -Werror \
   -I src \
   -I "$CJSON_DIR" \
   test/test_touch3.c \
   src/touch3.c \
   "$CJSON_DIR/cJSON.c" \
   -lm \
   -o "$OUT"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
