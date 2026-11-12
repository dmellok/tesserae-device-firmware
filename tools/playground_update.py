#!/usr/bin/env python3
"""Update the Tesserae entry in Seeed's reTerminal Sticky Playground registry.

Run by the `playground` job in .github/workflows/release.yml against a checkout
of https://github.com/Seeed-Projects/reterminal-sticky-playground-registry.
It does the mechanical part of a version update:

  - replaces the four images under firmwares/tesserae/firmware/<version>/
  - rewrites manifest.json (sizes and SHA-256) and the version in firmware.json
  - rewrites three README sections: "New since", "Package origin" and
    "Physical-device test record" (reset to pending)
  - writes a pull request description from the registry's template

What it cannot do is test the package on a device, so the pull request it
feeds is opened as a draft. Fill in the test record and trim the "New since"
list before marking it ready.

Exit status: 0 updated, 3 the registry already lists this version.

  playground_update.py --registry DIR --artifacts DIR --version 1.66.0 \
      --commit SHA --changelog CHANGELOG.md --pr-body OUT.md
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys

ENV = "seeed-reterminal-sticky"
ENTRY = "firmwares/tesserae"
UPSTREAM = "https://github.com/dmellok/tesserae-device-firmware"
PARTS = [  # manifest order
    ("bootloader", 0x0),
    ("partitions", 0x8000),
    ("ota_data_initial", 0x10000),
    ("firmware", 0x20000),
]

# A changelog entry whose lead-in ("Board name: ...") names one of these and
# that never mentions the Sticky is about a board that does not share the
# Sticky build, and is left out of the registry README.
OTHER_BOARDS = [
    "M5Stack", "M5Paper", "PaperS3", "PaperDIY", "PaperMono", "Paper Mono",
    "XIAO", "Inkplate", "CrowPanel", "epdiy", "Huessen", "LILYGO", "Waveshare",
    "Xteink", "OpenPaper", "E1001", "E1002", "E1003", "E1004", "EE02", "EE03",
    "EE04", "EE05", "Good Display", "GoodDisplay", "PhotoPainter", "RLCD",
]
STICKY_WORDS = ["Sticky", "reTerminals"]


def semver(v):
    return tuple(int(x) for x in v.split("."))


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def changelog_entries(text, after, upto):
    """[(version, [entry, ...])] for after < version <= upto, newest first."""
    out = []
    blocks = re.split(r"^## \[", text, flags=re.M)[1:]
    for block in blocks:
        head, _, body = block.partition("\n")
        m = re.match(r"(\d+\.\d+\.\d+)\]", head)
        if not m:
            continue  # [Unreleased]
        v = m.group(1)
        if not (semver(after) < semver(v) <= semver(upto)):
            continue
        entries, cur = [], None
        for line in body.splitlines():
            if line.startswith("- "):
                if cur is not None:
                    entries.append(cur)
                cur = line[2:].strip()
            elif cur is not None and line.startswith("  ") and line.strip():
                cur += " " + line.strip()
            elif cur is not None and not line.strip():
                entries.append(cur)
                cur = None
        if cur is not None:
            entries.append(cur)
        out.append((v, entries))
    return out


def for_sticky(entry):
    if any(w in entry for w in STICKY_WORDS):
        return True
    lead = entry.split(": ", 1)[0] if ": " in entry[:90] else ""
    return not any(b.lower() in lead.lower() for b in OTHER_BOARDS)


def replace_between(text, start, end, new):
    a = text.index(start)
    b = text.index(end, a)
    return text[:a] + new + text[b:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--registry", required=True)
    ap.add_argument("--artifacts", required=True)
    ap.add_argument("--version", required=True, help="without the leading v")
    ap.add_argument("--commit", required=True)
    ap.add_argument("--changelog", required=True)
    ap.add_argument("--pr-body", required=True)
    a = ap.parse_args()

    new = a.version.lstrip("v")
    entry = os.path.join(a.registry, ENTRY)
    fj_path = os.path.join(entry, "firmware.json")
    fj_text = open(fj_path).read()
    old = json.loads(fj_text)["flash"]["versions"][0]["version"]
    if old == new:
        print(f"registry already lists {new}; nothing to do")
        return 3
    if semver(new) < semver(old):
        sys.exit(f"registry lists {old}, newer than {new}")

    # Images. The previous version's directory is replaced, as in every
    # earlier update: firmware.json only ever lists the current version.
    old_dir = os.path.join(entry, "firmware", old)
    new_dir = os.path.join(entry, "firmware", new)
    same = {}
    os.makedirs(new_dir)
    manifest = json.load(open(os.path.join(old_dir, "manifest.json")))
    for name, _ in PARTS:
        src = os.path.join(a.artifacts, f"{ENV}-v{new}.part-{name}.bin")
        dst = os.path.join(new_dir, f"{name}.bin")
        shutil.copyfile(src, dst)
        prev = os.path.join(old_dir, f"{name}.bin")
        same[name] = os.path.exists(prev) and sha256(prev) == sha256(dst)
    shutil.rmtree(old_dir)

    manifest["version"] = new
    by_path = {p["path"]: p for p in manifest["builds"][0]["parts"]}
    hashes = {}
    for name, off in PARTS:
        p = by_path[f"{name}.bin"]
        f = os.path.join(new_dir, f"{name}.bin")
        if p["offset"] != off:
            sys.exit(f"{name}.bin: manifest offset {p['offset']:#x}, expected {off:#x}")
        p["size"] = os.path.getsize(f)
        p["sha256"] = hashes[name] = sha256(f)
    with open(os.path.join(new_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    if fj_text.count(old) != 2:
        sys.exit(f"firmware.json: expected {old} twice, found {fj_text.count(old)}")
    open(fj_path, "w").write(fj_text.replace(old, new))

    # README.
    log = changelog_entries(open(a.changelog).read(), old, new)
    kept, dropped = [], 0
    for v, entries in log:
        for e in entries:
            if for_sticky(e):
                kept.append((v, e))
            else:
                dropped += 1
    lines = [f"## New since {old}", ""]
    for v, e in kept:
        lines.append(f"- {e} ({v})")
    if not kept:
        lines.append("No changes that affect the Sticky build.")
    lines.append("")
    if dropped:
        lines += [
            f"{dropped} other change{'s' if dropped != 1 else ''} in this range "
            "are for boards that do not share the Sticky build.",
            "",
        ]
    new_since = "\n".join(lines) + "\n"

    unchanged = [f"`{n}.bin`" for n in ("partitions", "ota_data_initial", "bootloader") if same[n]]
    changed = [f"`{n}.bin`" for n in ("partitions", "ota_data_initial", "bootloader") if not same[n]]
    ident = ""
    if unchanged:
        ident += f"{' and '.join(unchanged)} {'are' if len(unchanged) > 1 else 'is'} byte-identical to the {old} package"
    if changed:
        ident += ("; " if ident else "") + f"{' and '.join(changed)} {'are new images' if len(changed) > 1 else 'is a new image'}"
    ident = ident[0].upper() + ident[1:] + "."

    origin = (
        "## Package origin\n\n"
        f"Version {new} was built by the tesserae-device-firmware GitHub Actions\n"
        "release pipeline from tag\n"
        f"[`v{new}`]({UPSTREAM}/releases/tag/v{new})\n"
        f"(commit `{a.commit}`), PlatformIO environment\n"
        f"`{ENV}`, ESP-IDF via PlatformIO `espressif32`. The four\n"
        f"files under `firmware/{new}/` are byte-identical to the images the pipeline\n"
        "publishes for the tesserae.ink browser flasher\n"
        f"(`https://tesserae.ink/firmware/{ENV}/v{new}/`), and the\n"
        "SHA-256 values in `manifest.json` match that catalog.\n"
        f"{ident} The firmware is distributed under the upstream AGPL-3.0 licence.\n\n"
    )

    record = (
        "## Physical-device test record\n\n"
        f"- Status: pending. The {new} package has not yet been installed on a\n"
        "  reTerminal Sticky; this record will be updated with the result before\n"
        "  review.\n\n"
    )

    rd_path = os.path.join(entry, "README.md")
    rd = open(rd_path).read()
    rd = replace_between(rd, "## New since ", "## Package origin", new_since)
    rd = replace_between(rd, "## Package origin", "| File | Offset", origin)
    rd = replace_between(rd, "## Physical-device test record",
                         "Project source, documentation and support", record)
    open(rd_path, "w").write(rd)

    # Pull request description, following the registry's template.
    part_lines = "\n".join(
        f"  - `{n}.bin` @ `{off:#x}`: `{hashes[n]}`" for n, off in PARTS)
    changes = "\n".join(f"- {e} ({v})" for v, e in kept) or "No changes that affect the Sticky build."
    body = f"""## Contribution type

- [ ] Firmware: new community firmware
- [ ] Firmware: new partner firmware coordinated with the platform owner
- [ ] Firmware: official Sticky firmware update maintained by Seeed
- [x] Firmware: update to an existing firmware
- [ ] 3D printable design: new design or update

## Common verification

- [x] `npm test` passes locally.
- [x] `npm run validate` passes locally.
- [x] The directory name and the `id` in the metadata file are the same lowercase kebab-case identifier.
- [x] All links use HTTPS and open the intended page.
- [x] The submitted files contain no user-specific credentials, API keys, tokens, passwords, or private keys.

The two checks above ran in the upstream release workflow that prepared this update, not on a workstation.

---

## Firmware

- Firmware name: Tesserae
- Directory: `{ENTRY}`
- Firmware version: {new} (replaces {old})
- Upstream project: {UPSTREAM}
- Source license (community and partner firmware-only packages): AGPL-3.0
- Firmware artifact origin and SHA-256 (firmware-only packages): built by the upstream GitHub Actions release pipeline from tag `v{new}` (commit `{a.commit[:7]}`), PlatformIO env `{ENV}`; identical to the images served by the tesserae.ink browser flasher.
{part_lines}

### Package type

- [ ] Source contribution built by GitHub Actions
- [x] Firmware-only package
- [ ] External community project (complete the application below)

### What changed since {old}

{changes}

{ident}

Full notes: {UPSTREAM}/blob/main/CHANGELOG.md

### Firmware verification

- [x] `firmware.json` uses the `group`, `catalogSection`, `mode`, and `category` documented in docs/contributing-firmware.md for the selected type (`partner` / `platform` / `flash`).
- [x] The README identifies the package origin, firmware version, and tested hardware.
- [x] Community entries include author attribution and a real Sticky preview; partner entries use the official logo as identity artwork.

Firmware-only package:

- [x] `source.url` points to the maintained upstream project and `source.license` names its license.
- [x] Every required `.bin` file is committed under `firmware/<version>/`.
- [x] The local manifest records every firmware file, flash offset, byte size, and SHA-256.

Update to an existing firmware:

- [x] The newest version appears first in `flash.versions`.
- [x] Every older version still referenced by `firmware.json` remains available. ({new} is the only version listed; {old} is replaced.)
- [x] Existing metadata and assets were updated only where the new version changes them.

### Physical-device test

- Device and hardware revision: pending
- Installation method: pending
- Tested firmware version: {new} (pending)
- Main workflow tested: pending
- Reboot and saved-state result: pending
- USB reconnection and repeated-install result: pending

- [ ] The submitted firmware-only package or a local build of the submitted source was installed on a physical reTerminal Sticky.
- [ ] First boot and the main user workflow passed.
- [ ] Touch and hardware buttons used by the firmware passed.
- [ ] Reboot, saved state, USB reconnection, and repeated installation were tested where applicable.

---

## Additional context

This draft was prepared by the upstream release workflow when `v{new}` was published. The on-device test is the open item: the README's test record and this description will be filled in, and review requested, once the package has run on a Sticky.
"""
    open(a.pr_body, "w").write(body)
    print(f"updated {ENTRY}: {old} -> {new} "
          f"({len(kept)} changelog entries kept, {dropped} left out)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
