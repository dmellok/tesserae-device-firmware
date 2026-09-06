#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
OUT="$(mktemp -t tesserae-maintenance-button.XXXXXX)"
trap 'rm -f "$OUT"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror -I include test/test_maintenance_button.c -o "$OUT"
"$OUT"
