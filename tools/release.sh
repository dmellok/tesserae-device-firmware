#!/usr/bin/env bash
#
# Cut a GitHub release for the current FW_VERSION in platformio.ini.
#
# What it does:
#   1. Reads FW_VERSION from platformio.ini.
#   2. Refuses to run if the working tree is dirty or if origin/main is ahead.
#   3. Builds the firmware fresh.
#   4. Copies the four .bin artifacts into release/<version>/ and writes
#      SHA256SUMS alongside them.
#   5. Tags vX.Y.Z (if not already tagged) and pushes the tag.
#   6. Creates / updates the GitHub release with the artifacts attached.
#
# Usage:
#   tools/release.sh                 # build, tag, release using FW_VERSION
#   tools/release.sh --notes-only    # just print suggested release notes
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# The macro is written as -DFW_VERSION=\"X.Y.Z\" in platformio.ini so the
# escaped quotes survive PlatformIO's shell-parsing. Strip backslashes from
# the captured field before using it as a tag name.
VERSION=$(awk -F'"' '/-DFW_VERSION/ {gsub(/\\/, "", $2); print $2; exit}' platformio.ini)
if [[ -z "${VERSION:-}" ]]; then
    echo "error: couldn't parse FW_VERSION from platformio.ini" >&2
    exit 1
fi
TAG="v${VERSION}"

if [[ "${1:-}" == "--notes-only" ]]; then
    echo "## tesserae-device-esp32-bin ${TAG}"
    echo
    git log --pretty=format:"- %s" "${TAG}~1..HEAD" 2>/dev/null \
        || git log --pretty=format:"- %s" -20
    exit 0
fi

# --- guardrails -------------------------------------------------------------
if [[ -n "$(git status --porcelain)" ]]; then
    echo "error: working tree is dirty; commit/stash first" >&2
    git status --short >&2
    exit 1
fi

git fetch origin --quiet
if ! git merge-base --is-ancestor origin/main HEAD; then
    echo "error: origin/main has commits this branch doesn't; pull/rebase first" >&2
    exit 1
fi

PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
ENV="${ENV:-tesserae-device-esp32-bin}"

# --- secrets.h isolation ----------------------------------------------------
# secrets.h is #include'd by app_config.h, so any compile-time defines in it
# get baked into the firmware as string literals (in .rodata) and would ship
# in the release artifacts. Move it aside for the build and restore on exit.
# A trap covers crashes, ^C, and any non-zero exit.
SECRETS="include/secrets.h"
SECRETS_BAK="include/.secrets.h.release-backup.$$"
if [[ -f "$SECRETS" ]]; then
    echo "==> moving ${SECRETS} aside for the build"
    mv "$SECRETS" "$SECRETS_BAK"
    trap 'mv "$SECRETS_BAK" "$SECRETS" 2>/dev/null || true' EXIT
fi

# --- build ------------------------------------------------------------------
echo "==> building ${ENV} for FW_VERSION=${VERSION}"
"$PIO" run -e "$ENV" >/dev/null

# --- secrets-leak scan ------------------------------------------------------
# Defense in depth: even if the move-aside above somehow failed, this catches
# any string literal from the (now-stashed) secrets.h that ended up baked
# into firmware.bin. Anything 4+ chars long is checked; shorter strings risk
# false-positive matches in unrelated code.
if [[ -f "$SECRETS_BAK" ]]; then
    echo "==> scanning firmware.bin for any string literal from secrets.h"
    LEAKED=0
    # grep -oE '"[^"]*"' pulls quoted literals out of the C source; sort -u
    # de-dupes. Then peel off the quotes with tr and length-filter.
    while IFS= read -r needle; do
        [[ ${#needle} -ge 4 ]] || continue
        if strings ".pio/build/${ENV}/firmware.bin" | grep -qF -- "$needle"; then
            echo "  LEAK: '$needle' from ${SECRETS} appears in firmware.bin" >&2
            LEAKED=1
        fi
    done < <(grep -oE '"[^"]+"' "$SECRETS_BAK" | tr -d '"' | sort -u)
    if (( LEAKED )); then
        echo "error: aborting release; secrets leaked into firmware.bin" >&2
        exit 1
    fi
    echo "  clean: no secrets.h literals found in firmware.bin"
fi

BUILD_DIR=".pio/build/${ENV}"
for f in bootloader.bin partitions.bin firmware.bin firmware.factory.bin; do
    [[ -f "$BUILD_DIR/$f" ]] || { echo "error: missing $BUILD_DIR/$f" >&2; exit 1; }
done

OUT_DIR="release/${VERSION}"
mkdir -p "$OUT_DIR"
cp "$BUILD_DIR"/bootloader.bin       "$OUT_DIR/"
cp "$BUILD_DIR"/partitions.bin       "$OUT_DIR/"
cp "$BUILD_DIR"/firmware.bin         "$OUT_DIR/"
cp "$BUILD_DIR"/firmware.factory.bin "$OUT_DIR/"

(cd "$OUT_DIR" && shasum -a 256 *.bin > SHA256SUMS)
echo "==> artifacts:"
ls -la "$OUT_DIR/"
echo "==> SHA256SUMS:"
cat "$OUT_DIR/SHA256SUMS"

# --- tag --------------------------------------------------------------------
if git rev-parse "$TAG" >/dev/null 2>&1; then
    echo "==> tag ${TAG} already exists; skipping tag step"
else
    echo "==> tagging ${TAG}"
    git tag -a "$TAG" -m "$TAG"
    git push origin "$TAG"
fi

# --- release ----------------------------------------------------------------
if gh release view "$TAG" >/dev/null 2>&1; then
    echo "==> release ${TAG} already exists; uploading (clobbering) assets"
    gh release upload "$TAG" "$OUT_DIR"/*.bin "$OUT_DIR/SHA256SUMS" --clobber
else
    echo "==> creating GitHub release ${TAG}"
    NOTES=$(mktemp)
    {
        echo "## Flashing"
        echo
        echo '`firmware.factory.bin` is the combined image; flash it to offset 0 with esptool:'
        echo
        echo '```bash'
        echo 'esptool.py --chip esp32s3 --port /dev/cu.usbmodem... \\'
        echo '    write_flash 0x0 firmware.factory.bin'
        echo '```'
        echo
        echo 'Or flash the three pieces separately at their native offsets:'
        echo
        echo '```bash'
        echo 'esptool.py --chip esp32s3 --port /dev/cu.usbmodem... \\'
        echo '    write_flash 0x0     bootloader.bin \\'
        echo '                0x8000  partitions.bin \\'
        echo '                0x10000 firmware.bin'
        echo '```'
        echo
        echo "## Checksums"
        echo
        echo '```'
        cat "$OUT_DIR/SHA256SUMS"
        echo '```'
        echo
        echo "## Changes"
        echo
        if PREV_TAG=$(git describe --tags --abbrev=0 "${TAG}^" 2>/dev/null); then
            git log --pretty=format:"- %s" "${PREV_TAG}..${TAG}^"
        else
            git log --pretty=format:"- %s"
        fi
    } > "$NOTES"

    gh release create "$TAG" \
        --title "$TAG" \
        --notes-file "$NOTES" \
        "$OUT_DIR"/*.bin "$OUT_DIR/SHA256SUMS"
    rm -f "$NOTES"
fi

echo "==> done"
