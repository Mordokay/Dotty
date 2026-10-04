#!/usr/bin/env python3
"""Builds every cartridge and the catalog the iOS app installs from.

  .venv/bin/python tools/build_catalog.py             # build into dist/
  .venv/bin/python tools/build_catalog.py --release   # ... and publish a GitHub Release

dist/ gets <id>-<version>.bin per cartridge plus catalog.json. A release is marked
"latest", so the app always finds the newest catalog at
  https://github.com/Mordokay/Dotty/releases/latest/download/catalog.json
The launcher is not in the catalog: it lives in the factory partition (USB only).

Each cartridges/<id>/ may have cartridge.json ({"description": ..., "requires": [...]})
and icon.png (64x64 1-bit after conversion, shown while installing).
"""

import argparse
import base64
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from dotty_image import REPO, NotACartridge, cartridge_icon_path, icon_bytes, read_cartridge_info

GITHUB_REPO = "Mordokay/Dotty"
CATALOG_FORMAT = 1
INSTALL_PROTOCOL = 1  # bump when the launcher's install protocol changes incompatibly
DIST = REPO / "dist"
PIO = os.environ.get("PIO", str(Path.home() / ".platformio/penv/bin/pio"))


def run(cmd, **kwargs):
    return subprocess.run(cmd, cwd=REPO, check=True, text=True, **kwargs)


def cartridge_ids():
    return sorted(p.name for p in (REPO / "cartridges").iterdir()
                  if (p / "main.cpp").exists() and p.name != "launcher")


def build(cartridge_id):
    print(f"building {cartridge_id}...", file=sys.stderr)
    result = subprocess.run([PIO, "run", "-e", cartridge_id], cwd=REPO, text=True, capture_output=True)
    if result.returncode != 0:
        sys.exit(f"{cartridge_id} failed to build:\n{result.stdout[-3000:]}{result.stderr[-2000:]}")
    image = (REPO / ".pio" / "build" / cartridge_id / "firmware.bin").read_bytes()
    try:
        info = read_cartridge_info(image)
    except NotACartridge as e:
        sys.exit(f"{cartridge_id}: {e}")
    if info["id"] != cartridge_id:
        sys.exit(f"cartridges/{cartridge_id} declares id '{info['id']}'; they must match")
    return image, info


def catalog_entry(cartridge_id, image, info, base_url):
    folder = REPO / "cartridges" / cartridge_id
    manifest_path = folder / "cartridge.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    icon_path = cartridge_icon_path(cartridge_id)
    filename = f"{info['id']}-{info['version']}.bin"
    (DIST / filename).write_bytes(image)
    return {
        **info,
        "description": manifest.get("description", ""),
        "requires": manifest.get("requires", []),
        "size": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "firmware": f"{base_url}/{filename}" if base_url else filename,
        "icon": base64.b64encode(icon_bytes(icon_path)).decode() if icon_path.exists() else None,
    }


def check_publishable():
    if run(["git", "status", "--porcelain"], capture_output=True).stdout.strip():
        sys.exit("commit your changes first: a release must match a pushed commit")
    head = run(["git", "rev-parse", "HEAD"], capture_output=True).stdout.strip()
    remote = run(["gh", "api", f"repos/{GITHUB_REPO}/commits/main", "--jq", ".sha"],
                 capture_output=True).stdout.strip()
    if head != remote:
        sys.exit("push first: HEAD is not what GitHub has on main")
    return head


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--release", action="store_true", help="publish dist/ as the latest GitHub Release")
    args = parser.parse_args()

    commit = check_publishable() if args.release else None
    now = datetime.datetime.now(datetime.timezone.utc)
    tag = now.strftime("cartridges-%Y.%m.%d-%H%M") if args.release else None
    base_url = f"https://github.com/{GITHUB_REPO}/releases/download/{tag}" if tag else None

    shutil.rmtree(DIST, ignore_errors=True)
    DIST.mkdir()
    entries = []
    for cartridge_id in cartridge_ids():
        image, info = build(cartridge_id)
        entries.append(catalog_entry(cartridge_id, image, info, base_url))

    catalog = {
        "format": CATALOG_FORMAT,
        "protocol": INSTALL_PROTOCOL,
        "generated": now.isoformat(timespec="seconds"),
        "release": tag,
        "cartridges": entries,
    }
    (DIST / "catalog.json").write_text(json.dumps(catalog, indent=2) + "\n")
    for e in entries:
        print(f"  {e['id']:10} {e['version']:8} {e['size'] // 1024:5} KB  {e['sha256'][:12]}…")

    if args.release:
        notes = "\n".join(f"- **{e['name']}** {e['version']} ({e['size'] // 1024} KB)" for e in entries)
        files = [str(DIST / "catalog.json")] + [str(DIST / Path(e["firmware"]).name) for e in entries]
        run(["gh", "release", "create", tag, *files, "--repo", GITHUB_REPO, "--target", commit,
             "--title", f"Cartridges {now:%Y-%m-%d %H:%M} UTC", "--notes", notes, "--latest"])
        print(f"published {tag}: https://github.com/{GITHUB_REPO}/releases/tag/{tag}")
    else:
        print(f"wrote {DIST}/ (use --release to publish)")


if __name__ == "__main__":
    main()
