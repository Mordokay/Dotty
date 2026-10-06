#!/usr/bin/env python3
"""Copies Dotty's whole SD card to this Mac, or back onto Dotty, over Wi-Fi.

  .venv/bin/python tools/card_backup.py backup [folder]   # default ~/Dotty Backups/<date time>
  .venv/bin/python tools/card_backup.py restore <folder>

Dotty switches to the launcher (whole-card transfers run there), joins its Wi-Fi and opens
its transfer server in whole-card mode (transfer.start {scope: card}); Bluetooth pauses
meanwhile. This Mac must be on the same Wi-Fi. Files go one by one (GET /download?path=,
POST /upload?path=), every size is checked, and the session ends with POST /done.
"""

import argparse
import datetime
import json
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
        if start >= 0:
            return json.loads(out[start:])
        time.sleep(3)  # Dotty may still be restarting
    return None


def ensure_launcher():
    info = ble("info")
    if info and info.get("role") == "launcher":
        return
    print("switching Dotty to the launcher…")
    ble("cmd", "core.toLauncher")
    for _ in range(20):
        time.sleep(2)
        info = ble("info")
        if info and info.get("role") == "launcher":
            return
    sys.exit("Dotty didn't come back as the launcher")


def open_session():
    ensure_launcher()
    print("Dotty is joining Wi-Fi…")
    reply = ble("cmd", "transfer.start", "scope=card", "--wait", "60", wait=60)
    if not reply or not reply.get("ok"):
        sys.exit(f"Dotty couldn't open the transfer: {reply and reply.get('error')}")
    print(f"  on {reply.get('ssid')} ({reply.get('rssi')} dBm) at {reply['url']}")
    return reply["url"], reply["token"]


def request(url, token, method="GET", data=None, timeout=60):
    req = urllib.request.Request(url, data=data, method=method, headers={"X-Dotty-Token": token})
    return urllib.request.urlopen(req, timeout=timeout)


def finish(url, token):
    try:
        request(f"{url}/done", token, method="POST", data=b"", timeout=10).read()
    except Exception:
        pass  # Dotty ends an idle session by itself after a minute


def backup(folder):
    url, token = open_session()
    try:
        files = json.loads(request(f"{url}/card/list", token).read())
        total = sum(f["size"] for f in files)
        print(f"{len(files)} files, {total / 1048576:.1f} MB → {folder}")
        done = 0
        started = time.time()
        for i, f in enumerate(files, 1):
            target = folder / f["path"].lstrip("/")
            target.parent.mkdir(parents=True, exist_ok=True)
            query = urllib.parse.urlencode({"path": f["path"]}, quote_via=urllib.parse.quote)
            with request(f"{url}/download?{query}", token, timeout=120) as response, open(target, "wb") as out:
                while chunk := response.read(65536):
                    out.write(chunk)
            if target.stat().st_size != f["size"]:
                sys.exit(f"{f['path']}: got {target.stat().st_size} of {f['size']} bytes")
            done += f["size"]
            rate = done / 1024 / max(1, time.time() - started)
            print(f"  {i}/{len(files)} {f['path']} ({rate:.0f} KB/s)")
        (folder / "dotty-backup.json").write_text(json.dumps(
            {"made": datetime.datetime.now().isoformat(timespec="seconds"), "files": files}, indent=2))
        print(f"done: {len(files)} files in {time.time() - started:.0f} s")
    finally:
        finish(url, token)


def restore(folder):
    files = [p for p in sorted(folder.rglob("*")) if p.is_file() and p.name != "dotty-backup.json"
             and not p.name.startswith(".")]
    if not files:
        sys.exit(f"nothing to restore in {folder}")
    url, token = open_session()
    try:
        started = time.time()
        for i, p in enumerate(files, 1):
            path = "/" + p.relative_to(folder).as_posix()
            query = urllib.parse.urlencode({"path": path}, quote_via=urllib.parse.quote)
            with request(f"{url}/upload?{query}", token, method="POST", data=p.read_bytes(), timeout=180) as r:
                reply = json.loads(r.read())
            if not reply.get("ok"):
                sys.exit(f"{path}: {reply.get('error')}")
            print(f"  {i}/{len(files)} {path}")
        print(f"done: {len(files)} files in {time.time() - started:.0f} s")
    finally:
        finish(url, token)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="action", required=True)
    b = sub.add_parser("backup")
    b.add_argument("folder", nargs="?", type=Path)
    r = sub.add_parser("restore")
    r.add_argument("folder", type=Path)
    args = parser.parse_args()
    if args.action == "backup":
        folder = args.folder or Path.home() / "Dotty Backups" / datetime.datetime.now().strftime("%Y-%m-%d %H%M")
        folder.mkdir(parents=True, exist_ok=True)
        backup(folder)
    else:
        restore(args.folder)


if __name__ == "__main__":
    main()
