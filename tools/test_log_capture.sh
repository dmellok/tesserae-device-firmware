#!/bin/sh
# Host-side tests for device log capture (ring, redaction, upload batch) and
# the detected-failure latch reported as "diag" on /status.
set -eu

OUT="$(mktemp -d -t tesserae-log-capture.XXXXXX)"
cc -std=c11 -Wall -Wextra -Werror -I src \
   test/test_log_ring.c src/log_ring.c -o "$OUT/test_log_ring"
cc -std=c11 -Wall -Wextra -Werror -I src \
   test/test_diag.c src/diag.c -o "$OUT/test_diag"
"$OUT/test_log_ring"
"$OUT/test_diag"
