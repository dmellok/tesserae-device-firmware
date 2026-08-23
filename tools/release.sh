#!/usr/bin/env bash
#
# Cut a release.
#
#   tools/release.sh 1.19.0 notes.md "the Sticky reports its battery"
#
# Arguments: version (no leading v), a file holding the release notes, and an
# optional one-line summary that becomes the release title after the tag.
#
# WHAT THIS DOES, AND WHAT IT DELIBERATELY DOES NOT.
#
# Building and publishing happen in CI: .github/workflows/release.yml fires on
# the published release, builds every target in its matrix, merges each into a
# factory image, publishes the images plus catalog.json to the R2 bucket the
# tesserae.ink flasher reads, and (after review, in the ota-signing environment)
# signs the OTA descriptors. That is fifteen targets and a signing step; none of
# it can usefully be reproduced from a laptop.
#
# So the only part that has to happen locally is deciding that the tree is fit
# to release, and creating the tag. That is all this script does. It is mostly
# guardrails, which is the point: the previous version of this script built one
# environment by hand and attached four .bin files, and had drifted so far that
# it would have tagged v99.0.0-bench (the version it parsed out of the bench
# env's build flags) using an ENV name that no longer exists.
#
# THE VERSION IS THE TAG. Nothing in the repo records it. release.yml bakes
# FW_VERSION from the tag it was triggered by, which is why this script takes
# the version as an argument and refuses to guess.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

die() { echo "error: $*" >&2; exit 1; }

VERSION="${1:-}"
NOTES_FILE="${2:-}"
SUMMARY="${3:-}"

if [[ -z "$VERSION" || -z "$NOTES_FILE" ]]; then
    cat >&2 <<'EOF'
usage: tools/release.sh <version> <notes-file> [summary]

  version     X.Y.Z, no leading v. This becomes the tag, and CI bakes it into
              FW_VERSION. Nothing in the repo records it.
  notes-file  file holding the release notes body
  summary     optional one-liner; the title becomes "vX.Y.Z: <summary>"

example: tools/release.sh 1.19.0 notes.md "the Sticky reports its battery"
EOF
    exit 1
fi

# --- guardrails -------------------------------------------------------------
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] \
    || die "version must be X.Y.Z with no leading v (got '${VERSION}')"
TAG="v${VERSION}"

[[ -f "$NOTES_FILE" && -s "$NOTES_FILE" ]] \
    || die "notes file '${NOTES_FILE}' is missing or empty"

[[ -z "$(git status --porcelain)" ]] \
    || { git status --short >&2; die "working tree is dirty; commit or stash first"; }

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
[[ "$BRANCH" == "main" ]] \
    || die "on branch '${BRANCH}'; releases are cut from main"

git fetch origin --quiet
[[ "$(git rev-parse HEAD)" == "$(git rev-parse origin/main)" ]] \
    || die "main and origin/main differ; pull or push first"

! git rev-parse "$TAG" >/dev/null 2>&1 \
    || die "tag ${TAG} already exists"

gh release view "$TAG" >/dev/null 2>&1 \
    && die "release ${TAG} already exists"

TITLE="$TAG"
[[ -n "$SUMMARY" ]] && TITLE="${TAG}: ${SUMMARY}"

# --- confirm ----------------------------------------------------------------
# Publishing is one-way: it triggers the flasher publish to R2 and queues OTA
# signing, so the last chance to notice a wrong version or stale notes is here.
cat >&2 <<EOF

  tag      ${TAG}
  title    ${TITLE}
  commit   $(git log --oneline -1)
  notes    ${NOTES_FILE} ($(wc -l < "$NOTES_FILE" | tr -d ' ') lines)

Publishing builds and ships every target to the flasher. Continue? [y/N]
EOF
read -r reply
[[ "$reply" == "y" || "$reply" == "Y" ]] || die "aborted"

# --- release ----------------------------------------------------------------
gh release create "$TAG" \
    --target main \
    --title "$TITLE" \
    --notes-file "$NOTES_FILE"

echo "==> published ${TAG}; watch the build with:"
echo "    gh run watch \$(gh run list --workflow=release.yml --limit 1 --json databaseId -q '.[0].databaseId')"
