#!/usr/bin/env python3
"""Copies Dotty's whole SD card to this Mac, or back onto Dotty, over Wi-Fi.

  .venv/bin/python tools/card_backup.py backup [folder]   # default ~/Dotty Backups/<date time>
  .venv/bin/python tools/card_backup.py backup --with-firmware   # also the cartridge copies
  .venv/bin/python tools/card_backup.py backup --only weather,jokes   # just those cartridges
  .venv/bin/python tools/card_backup.py restore <folder> [--dry-run]

A restore makes the card match the backup but only moves what differs: Dotty lists its
files with their SHA-256 (GET /card/list?hash=1), and only new or changed files are sent;
files the backup doesn't have are deleted, but only inside the cartridges the backup
holds (a Weather-only backup leaves Music alone). Never deleted: the launcher's copies (Rescue
needs them), hidden files, and the firmware copies when the backup has only data.

By default only the data is copied (songs, photos, recordings, jokes…): the cartridge
firmware copies (/cartridges/<id>/firmware/) come back from the catalog, and Rescue keeps
the launcher's through a factory reset.

Dotty switches to the launcher (whole-card transfers run there), joins its Wi-Fi and opens
its transfer server in whole-card mode (transfer.start {scope: card}); Bluetooth pauses
meanwhile. This Mac must be on the same Wi-Fi. Files go one by one (GET /download?path=,
POST /upload?path=), every size is checked, and the session ends with POST /done.
"""

import argparse
import urllib.error
import datetime
import hashlib
import http.client
import json
import shutil
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BLE = [sys.executable, str(REPO / "tools" / "ble_dotty.py")]


def ble(*args, wait=60):
    """Runs a ble_dotty.py command and returns its JSON output (None if it printed none)."""
    for attempt in range(3):
        out = subprocess.run(BLE + list(args), capture_output=True, text=True, timeout=wait + 30).stdout
        start = out.find("{")
        if start >= 0:  # the first JSON object (anything after it is other output)
            return json.JSONDecoder().raw_decode(out[start:])[0]
        time.sleep(3)  # Dotty may still be restarting
    return None


def ensure_launcher():
    info = ble("info")
    if info and info.get("role") == "launcher":
        return
    print("switching Dotty to the launcher…")
    for _ in range(6):  # a command can get lost while the Mac (re)pairs: ask again
        ble("cmd", "core.toLauncher")
        time.sleep(8)  # it restarts into the launcher
        info = ble("info")
        if info and info.get("role") == "launcher":
            return
    sys.exit("Dotty didn't come back as the launcher")


def open_session():
    ensure_launcher()
    print("Dotty is joining Wi-Fi…")
    reply = ble("cmd", "transfer.start", "scope=card", "--wait", "120", wait=120)
    if not reply or not reply.get("ok"):
        sys.exit(f"Dotty couldn't open the transfer: {reply and reply.get('error')}")
    print(f"  on {reply.get('ssid')} ({reply.get('rssi')} dBm) at {reply['url']}")
    return reply["url"], reply["token"]


def request(url, token, method="GET", data=None, timeout=60, progress=None):
    """progress = (job, step, steps, bytes before, bytes total): shown on Dotty's screen."""
    headers = {"X-Dotty-Token": token}
    if progress:
        job, step, steps, before, total = progress
        headers.update({"X-Dotty-Job": job, "X-Dotty-Step": f"{step}/{steps}", "X-Dotty-Bytes": f"{before}/{total}"})
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    return urllib.request.urlopen(req, timeout=timeout)


def cancelled_on_dotty(url, token):
    """True when the transfer was stopped with Dotty's Cancel button."""
    try:
        return json.loads(request(f"{url}/status", token, timeout=5).read()).get("cancelled", False)
    except Exception:
        return False


def finish(url, token):
    try:
        request(f"{url}/done", token, method="POST", data=b"", timeout=10).read()
    except Exception:
        pass  # Dotty ends an idle session by itself after a minute


def cartridge_of(path):
    """'/cartridges/music/data/…' → 'music'."""
    parts = Path(path).parts
    return parts[2] if len(parts) > 3 and parts[1] == "cartridges" else None


def backup(folder, with_firmware=False, only=None):
    url, token = open_session()
    try:
        files = json.loads(request(f"{url}/card/list", token).read())
        files = [f for f in files if not hidden(f["path"])  # e.g. a Mac's .Spotlight-V100
                 and cartridge_of(f["path"]) not in (None, "launcher")]
        if only:
            files = [f for f in files if cartridge_of(f["path"]) in only]
        if not with_firmware:
            files = [f for f in files if "/firmware/" not in f["path"]]
        total = sum(f["size"] for f in files)
        print(f"{len(files)} files, {total / 1048576:.1f} MB → {folder}")
        started = time.time()
        try:
            copy(url, token, files, folder, total, started)
        except (urllib.error.URLError, OSError, http.client.HTTPException) as error:
            shutil.rmtree(folder, ignore_errors=True)  # no half backups
            stopped(url, token, error)
        (folder / "dotty-backup.json").write_text(json.dumps(
            {"made": datetime.datetime.now().isoformat(timespec="seconds"),
             "scope": "all" if with_firmware else "data",
             "cartridges": sorted({cartridge_of(f["path"]) for f in files}), "files": files}, indent=2))
        print(f"done: {len(files)} files in {time.time() - started:.0f} s")
    finally:
        finish(url, token)


def copy(url, token, files, folder, total, started):
    done = 0
    for i, f in enumerate(files, 1):
        target = folder / f["path"].lstrip("/")
        target.parent.mkdir(parents=True, exist_ok=True)
        query = urllib.parse.urlencode({"path": f["path"]}, quote_via=urllib.parse.quote)
        with request(f"{url}/download?{query}", token, timeout=120,
                     progress=("backup", i, len(files), done, total)) as response, open(target, "wb") as out:
            while chunk := response.read(65536):
                out.write(chunk)
        if target.stat().st_size != f["size"]:
            sys.exit(f"{f['path']}: got {target.stat().st_size} of {f['size']} bytes")
        done += f["size"]
        rate = done / 1024 / max(1, time.time() - started)
        print(f"  {i}/{len(files)} {f['path']} ({rate:.0f} KB/s)")


def hidden(path):
    return any(part.startswith(".") for part in Path(path).parts)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def restore(folder, dry_run=False):
    manifest = folder / "dotty-backup.json"
    info = json.loads(manifest.read_text()) if manifest.exists() else {}
    scope = info.get("scope", "all")
    local = {"/" + p.relative_to(folder).as_posix(): p for p in sorted(folder.rglob("*"))
             if p.is_file() and p != manifest and not hidden(p.relative_to(folder))}
    # older backups don't list their cartridges: every one they have files of
    cartridges = set(info.get("cartridges") or filter(None, map(cartridge_of, local)))
    if not local:
        sys.exit(f"nothing to restore in {folder}")
    url, token = open_session()
    try:
        print("Dotty is fingerprinting its files…")
        with request(f"{url}/card/list?hash=1", token, timeout=600) as r:
            remote = {f["path"]: f for f in json.loads(r.read())}
        send, same = [], 0
        for path, p in local.items():
            there = remote.get(path)
            if there and there["size"] == p.stat().st_size and there.get("sha256") == sha256(p):
                same += 1
            else:
                send.append(path)

        def keep(path):  # what a restore never deletes
            return (hidden(path) or path.startswith("/cartridges/launcher/")
                    or (scope == "data" and "/firmware/" in path)
                    or cartridge_of(path) not in cartridges)

        delete = [path for path in remote if path not in local and not keep(path)]
        size = sum(local[p].stat().st_size for p in send)
        print(f"{same} files already match, {len(send)} to send ({size / 1048576:.1f} MB), {len(delete)} to delete")
        if dry_run:
            for path in send:
                print(f"  send   {path}")
            for path in delete:
                print(f"  delete {path}")
            return
        started = time.time()
        try:
            put(url, token, local, send, delete, size)
        except (urllib.error.URLError, OSError, http.client.HTTPException) as error:
            stopped(url, token, error)
        print(f"done in {time.time() - started:.0f} s")
    finally:
        finish(url, token)


def put(url, token, local, send, delete, size):
    sent = 0
    for i, path in enumerate(send, 1):
        query = urllib.parse.urlencode({"path": path}, quote_via=urllib.parse.quote)
        data = local[path].read_bytes()
        with request(f"{url}/upload?{query}", token, method="POST", data=data, timeout=180,
                     progress=("restore", i, len(send), sent, size)) as r:
            reply = json.loads(r.read())
        if not reply.get("ok"):
            sys.exit(f"{path}: {reply.get('error')}")
        sent += len(data)
        print(f"  sent {i}/{len(send)} {path}")
    for path in delete:
        query = urllib.parse.urlencode({"path": path}, quote_via=urllib.parse.quote)
        request(f"{url}/card/delete?{query}", token, method="POST", data=b"").read()
        print(f"  deleted {path}")


def stopped(url, token, error):
    """Turns an error into 'stopped on Dotty' when someone pressed Cancel there."""
    if (isinstance(error, urllib.error.HTTPError) and error.code == 409) or cancelled_on_dotty(url, token):
        sys.exit("stopped on Dotty")
    raise error


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="action", required=True)
    b = sub.add_parser("backup")
    b.add_argument("folder", nargs="?", type=Path)
    b.add_argument("--with-firmware", action="store_true", help="also the cartridge firmware copies")
    b.add_argument("--only", help="comma-separated cartridge ids (default: all)")
    r = sub.add_parser("restore")
    r.add_argument("folder", type=Path)
    r.add_argument("--dry-run", action="store_true", help="only show what would change")
    args = parser.parse_args()
    if args.action == "backup":
        folder = args.folder or Path.home() / "Dotty Backups" / datetime.datetime.now().strftime("%Y-%m-%d %H%M")
        folder.mkdir(parents=True, exist_ok=True)
        backup(folder, args.with_firmware, set(args.only.split(",")) if args.only else None)
    else:
        restore(args.folder, args.dry_run)


if __name__ == "__main__":
    main()
