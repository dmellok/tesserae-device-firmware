#!/usr/bin/env python3
"""Assemble the firmware catalog the tesserae.ink flasher reads.

Reads the per-target `<env>.meta.json` fragments staged by the release workflow,
merges the new versions into the existing catalog pulled from R2, and writes:

  - catalog.json        the merged catalog (uploaded to R2 at /firmware/catalog.json)
  - upload_manifest.tsv  "<local factory.bin>\t<r2 key>" lines for the upload step

Catalog shape:
  {
    "updated": "<tag>",
    "targets": {
      "<env>": {
        "chipFamily": "ESP32-S3",
        "offset": 0,
        "webSerial": true,
        "versions": [ {"version": "v1.0.0", "path": "<env>/v1.0.0.factory.bin", "sha256": "..."}, ... ]
      }
    }
  }

Newest version is kept first; history is capped so the dropdown stays sane.
"""
import glob
import json
import os
import sys

MAX_VERSIONS = 10


def main() -> int:
    if len(sys.argv) != 5:
        print("usage: build_catalog.py <artifacts_dir> <existing_catalog> <out_catalog> <tag>")
        return 2
    art_dir, existing_path, out_path, tag = sys.argv[1:5]

    try:
        with open(existing_path) as f:
            catalog = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        catalog = {}
    if not isinstance(catalog, dict):
        catalog = {}
    targets = catalog.setdefault("targets", {})

    uploads = []
    metas = sorted(glob.glob(os.path.join(art_dir, "*.meta.json")))
    if not metas:
        print(f"no *.meta.json fragments in {art_dir}", file=sys.stderr)
        return 1

    for meta_file in metas:
        with open(meta_file) as f:
            m = json.load(f)
        env = m["env"]
        t = targets.setdefault(env, {})
        t["chipFamily"] = m["chipFamily"]
        t["offset"] = m["offset"]
        t["webSerial"] = m["webSerial"]
        versions = [v for v in t.get("versions", []) if v.get("version") != m["version"]]
        versions.insert(0, {"version": m["version"], "path": m["path"], "sha256": m["sha256"]})
        t["versions"] = versions[:MAX_VERSIONS]

        local_bin = os.path.join(art_dir, f'{env}-{m["version"]}.factory.bin')
        uploads.append((local_bin, m["path"]))

    catalog["updated"] = tag
    with open(out_path, "w") as f:
        json.dump(catalog, f, indent=2)
        f.write("\n")
    with open("upload_manifest.tsv", "w") as f:
        for local, key in uploads:
            f.write(f"{local}\t{key}\n")

    print(f"catalog: {len(targets)} targets total, {len(uploads)} bins to upload")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
