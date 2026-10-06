#!/usr/bin/env python3
"""Builds every cartridge and the catalog the iOS app installs from.

  .venv/bin/python tools/build_catalog.py             # build into dist/
  .venv/bin/python tools/build_catalog.py --release   # ... and publish a GitHub Release

dist/ gets <id>-<version>.bin per cartridge plus catalog.json. A release is marked
"latest", so the app always finds the newest catalog at
  https://github.com/Mordokay/Dotty/releases/latest/download/catalog.json
The launcher is in the catalog too, marked "system": true (apps don't list it as a
cartridge). Installed like a cartridge, it copies itself into the factory partition at its
first start (cartridges/launcher/main.cpp, installSelfIfUpdate): that's how Dotty's system
firmware updates from the app. Since flash layout 2 Rescue installs it (cartridges/rescue/),
and Rescue itself is never in the catalog.

Each cartridges/<id>/ may have cartridge.json ({"description": ..., "requires": [...]}),
icon.png (64x64 1-bit after conversion: Dotty's install screen) and artwork.png (square,
full colour, 512x512 or larger: the app's picture of the cartridge, published as
<id>-<version>.png). artwork.svg, when present, is its editable source; re-render with
  qlmanage -t -s 512 -o /tmp cartridges/<id>/artwork.svg && mv /tmp/artwork.svg.png cartridges/<id>/artwork.png
"""

import argparse
import io
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
    # Rescue (factory partition) is USB-only: never in the catalog.
    return sorted(p.name for p in (REPO / "cartridges").iterdir()
                  if (p / "main.cpp").exists() and p.name != "rescue")


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


ARTWORK_SIDE = 512


def write_artwork(source, target):
    """Square PNG, scaled to ARTWORK_SIDE so the app never downloads more than it shows."""
    from PIL import Image
    with Image.open(source) as art:
        if art.width != art.height:
            sys.exit(f"{source}: artwork must be square (is {art.width}x{art.height})")
        art = art.convert("RGBA")
        if art.width > ARTWORK_SIDE:
            art = art.resize((ARTWORK_SIDE, ARTWORK_SIDE), Image.LANCZOS)
        buffer = io.BytesIO()
        art.save(buffer, "PNG", optimize=True)
        target.write_bytes(buffer.getvalue())


def catalog_entry(cartridge_id, image, info, base_url):
    folder = REPO / "cartridges" / cartridge_id
    manifest_path = folder / "cartridge.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    icon_path = cartridge_icon_path(cartridge_id)
    filename = f"{info['id']}-{info['version']}.bin"
    (DIST / filename).write_bytes(image)
    artwork = None
    if (folder / "artwork.png").exists():
        artwork = f"{info['id']}-{info['version']}.png"
        write_artwork(folder / "artwork.png", DIST / artwork)
    return {
        **info,
        "description": manifest.get("description", ""),
        "requires": manifest.get("requires", []),
        "size": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "firmware": f"{base_url}/{filename}" if base_url else filename,
        "icon": base64.b64encode(icon_bytes(icon_path)).decode() if icon_path.exists() else None,
        "artwork": (f"{base_url}/{artwork}" if base_url else artwork) if artwork else None,
        **({"system": True} if cartridge_id == "launcher" else {}),
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
        files += [str(DIST / Path(e["artwork"]).name) for e in entries if e["artwork"]]
        run(["gh", "release", "create", tag, *files, "--repo", GITHUB_REPO, "--target", commit,
             "--title", f"Cartridges {now:%Y-%m-%d %H:%M} UTC", "--notes", notes, "--latest"])
        print(f"published {tag}: https://github.com/{GITHUB_REPO}/releases/tag/{tag}")
    else:
        print(f"wrote {DIST}/ (use --release to publish)")


if __name__ == "__main__":
    main()
