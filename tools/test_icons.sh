#!/bin/sh
# Host-side touch-v3 icon chain test: the generated Phosphor codepoint map, the
# vendored font binary, and the stb_truetype rasterizer, checked together.
# Catches a map regenerated from a different Phosphor release than the bundled
# font -- the glyph-mismatch failure the contract's version pin exists to
# prevent, which otherwise only shows up as wrong pictures on a panel.
set -eu

OUT="$(mktemp -t tesserae-icons.XXXXXX)"
# stb_truetype is third-party: compile it without our warning flags.
cc -std=c11 -O1 -w \
   -I src \
   -I include \
   test/test_icons.c \
   -lm \
   -o "$OUT"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
