#!/usr/bin/env python3
"""Talk to Dotty over BLE from the Mac, the same way the iOS app will (Dotty Core service).

Setup once:
  ~/.platformio/penv/bin/uv venv .venv --python ~/.platformio/penv/bin/python
  ~/.platformio/penv/bin/uv pip install --python .venv/bin/python bleak pillow

Examples:
  .venv/bin/python tools/ble_dotty.py scan
  .venv/bin/python tools/ble_dotty.py info
  .venv/bin/python tools/ble_dotty.py cmd music.volume value:=60
  .venv/bin/python tools/ble_dotty.py cmd wifi.set ssid="My Wi-Fi" password="secret"
  .venv/bin/python tools/ble_dotty.py cmd library.fetch id=music      # Dotty downloads it over Wi-Fi
  .venv/bin/python tools/ble_dotty.py cmd core.toLauncher
  .venv/bin/python tools/ble_dotty.py listen
  .venv/bin/python tools/ble_dotty.py install .pio/build/music/firmware.bin
  .venv/bin/python tools/ble_dotty.py catalog
  .venv/bin/python tools/ble_dotty.py install music      # latest from the GitHub catalog

The first run asks macOS for Bluetooth permission for your terminal app.
"""

import argparse
import asyncio
import base64
import calendar
import hashlib
import json
import struct
import sys
import time
import urllib.request
from pathlib import Path

from bleak import BleakClient, BleakScanner

from dotty_image import NotACartridge, cartridge_icon_path, icon_bytes, read_cartridge_info

SERVICE = "b9c10000-fbaa-4525-8400-055f7a543231"
INFO = "b9c10001-fbaa-4525-8400-055f7a543231"
COMMAND = "b9c10002-fbaa-4525-8400-055f7a543231"
EVENT = "b9c10003-fbaa-4525-8400-055f7a543231"
DATA = "b9c10004-fbaa-4525-8400-055f7a543231"
CATALOG_URL = "https://github.com/Mordokay/Dotty/releases/latest/download/catalog.json"


def serial_from(adv):
    """Chip serial from the manufacturer data (company 0xFFFF + 6 bytes)."""
    data = adv.manufacturer_data.get(0xFFFF)
    return ":".join(f"{b:02X}" for b in data) if data and len(data) == 6 else "?"


def advertised_name(device, adv):
    """The name Dotty advertises now (macOS caches device.name: Dotty-B100 after the rename)."""
    return adv.local_name or device.name or ""


async def find(name, timeout):
    devices = await BleakScanner.discover(timeout=timeout, service_uuids=[SERVICE], return_adv=True)
    wanted = (name or "").lower()
    found = [(d, adv) for d, adv in devices.values()
             if not wanted or wanted in (advertised_name(d, adv).lower(), serial_from(adv).lower())]
    if not found:
        sys.exit("No Dotty found. Is it unlocked (Bluetooth is off while locked and asleep)?")
    return found


def build_command(cmd, params):
    if cmd.startswith("{"):
        return json.loads(cmd)
    message = {"cmd": cmd}
    for param in params:
        # httpie style: key=value is always a string, key:=value is raw JSON (numbers,
        # booleans), so e.g. an all-digit Wi-Fi password stays a string.
        if ":=" in param:
            key, _, value = param.partition(":=")
            message[key] = json.loads(value)
        else:
            key, _, value = param.partition("=")
            message[key] = value
    return message


async def scan(args):
    for device, adv in await find(args.name, args.timeout):
        print(f"{advertised_name(device, adv) or '?':12} serial {serial_from(adv)}  RSSI {adv.rssi} dBm")


async def connect(args):
    device, _ = (await find(args.name, args.timeout))[0]
    client = BleakClient(device)
    await client.connect()
    print(f"connected to {device.name}", file=sys.stderr)
    return client


async def info(args):
    async with await connect(args) as client:
        raw = await client.read_gatt_char(INFO)
        print(json.dumps(json.loads(raw), indent=2))


MORE_FOLLOWS = 0x1E


def whole_messages(handler):
    """Wraps an Event callback: long messages arrive in pieces, each but the last starting
    with 0x1E; the handler only sees complete messages."""
    pending = bytearray()

    def on_piece(sender, data):
        if data and data[0] == MORE_FOLLOWS:
            pending.extend(data[1:])
            return
        message = bytes(pending) + bytes(data)
        pending.clear()
        handler(sender, message)

    return on_piece


async def command(args):
    message = build_command(args.cmd, args.params)
    reply = asyncio.get_running_loop().create_future()

    def on_event(_, data):
        event = json.loads(data)
        if event.get("cmd") == message["cmd"] and not reply.done():
            reply.set_result(event)
        else:
            print("event:", json.dumps(event), file=sys.stderr)

    async with await connect(args) as client:
        await client.start_notify(EVENT, whole_messages(on_event))
        if message["cmd"] == "core.time" and "local" not in message:
            # The Mac's local time, taken after connecting and pairing (a ping first), so it
            # isn't stale by the time it arrives.
            await client.write_gatt_char(COMMAND, b'{"cmd":"core.ping"}', response=True)
            now = time.time()
            message["local"] = calendar.timegm(time.localtime(now))
        await client.write_gatt_char(COMMAND, json.dumps(message).encode(), response=True)
        try:
            print(json.dumps(await asyncio.wait_for(reply, args.wait), indent=2))
        except asyncio.TimeoutError:
            print(f"no reply within {args.wait:g} s (the device may have rebooted)", file=sys.stderr)


async def listen(args):
    async with await connect(args) as client:
        await client.start_notify(EVENT, whole_messages(lambda _, data: print(data.decode())))
        print("listening, Ctrl+C to stop", file=sys.stderr)
        while client.is_connected:
            await asyncio.sleep(1)


def write_queue_ready(client):
    """True when CoreBluetooth can take another unacknowledged write. bleak doesn't
    check this on macOS, and writes sent while the queue is full are silently dropped
    (the iOS app must check canSendWriteWithoutResponse the same way)."""
    peripheral = getattr(getattr(client, "_backend", None), "_peripheral", None)
    return peripheral is None or peripheral.canSendWriteWithoutResponse()


async def ensure_launcher(args):
    """Connects; if a cartridge is running, sends it back to the launcher first."""
    client = await connect(args)
    role = json.loads(await client.read_gatt_char(INFO)).get("role")
    if role == "launcher":
        return client
    print("cartridge running, switching to the launcher...", file=sys.stderr)
    await client.write_gatt_char(COMMAND, json.dumps({"cmd": "core.toLauncher"}).encode(), response=True)
    await client.disconnect()
    await asyncio.sleep(6)
    return await connect(args)


def fetch_catalog(url):
    with urllib.request.urlopen(url, timeout=20) as response:
        return json.load(response)


async def catalog(args):
    data = fetch_catalog(args.catalog)
    print(f"catalog {data.get('release', '?')} ({data['generated']}):")
    for entry in data["cartridges"]:
        print(f"  {entry['id']:10} {entry['name']} {entry['version']:8} {entry['size'] // 1024:5} KB  "
              f"{entry.get('description', '')}")


def load_image(args):
    """(image, meta, icon): from a local .bin, or downloaded from the catalog by id."""
    path = Path(args.firmware)
    if path.exists():
        image = path.read_bytes()
        try:
            meta = read_cartridge_info(image)
        except NotACartridge as e:
            sys.exit(f"{path}: {e}")
        icon_path = Path(args.icon) if args.icon else cartridge_icon_path(meta["id"])
        return image, meta, icon_bytes(icon_path) if icon_path.exists() else b""

    data = fetch_catalog(args.catalog)
    entry = next((e for e in data["cartridges"] if e["id"] == args.firmware), None)
    if not entry:
        sys.exit(f"'{args.firmware}' is neither a file nor a cartridge in the catalog")
    print(f"downloading {entry['firmware']}...", file=sys.stderr)
    with urllib.request.urlopen(entry["firmware"], timeout=60) as response:
        image = response.read()
    if hashlib.sha256(image).hexdigest() != entry["sha256"]:
        sys.exit("downloaded image does not match the catalog's SHA-256")
    meta = {k: entry[k] for k in ("id", "name", "version")}
    return image, meta, base64.b64decode(entry["icon"]) if entry.get("icon") else b""


async def install(args):
    image, meta, icon = load_image(args)
    sha256 = hashlib.sha256(image).hexdigest()
    if args.corrupt:  # test the launcher's integrity check: one flipped byte
        image = image[:1000] + bytes([image[1000] ^ 0xFF]) + image[1001:]
    payload = icon + image  # the launcher splits the icon off the front
    print(f"{meta['name']} {meta['version']}: {len(image) // 1024} KB{'' if icon else ', no icon'}",
          file=sys.stderr)

    client = await ensure_launcher(args)
    replies = {}
    state = {"received": 0, "rewind": None, "rewinds": 0}
    changed = asyncio.Event()

    def on_event(_, data):
        event = json.loads(data)
        kind = event.get("event")
        if kind == "install.progress":
            state["received"] = event["received"]
        elif kind == "install.resend":
            state["rewind"] = event["from"]
        elif "cmd" in event:
            replies[event["cmd"]] = event
        changed.set()

    async def wait_change(timeout):
        changed.clear()
        await asyncio.wait_for(changed.wait(), timeout)

    async def request(message, timeout=30):
        replies.pop(message["cmd"], None)
        await client.write_gatt_char(COMMAND, json.dumps(message).encode(), response=True)
        deadline = time.monotonic() + timeout
        while message["cmd"] not in replies:
            await wait_change(max(0.1, deadline - time.monotonic()))
        return replies[message["cmd"]]

    async with client:
        await client.start_notify(EVENT, whole_messages(on_event))
        reply = await request({"cmd": "install.begin", **meta, "size": len(image),
                               "sha256": sha256, "icon": bool(icon)})
        if not reply.get("ok"):
            sys.exit(f"install.begin failed: {reply.get('error')}")
        window = reply["window"]
        chunk = client.mtu_size - 3 - 4  # ATT header, then our 4-byte offset
        start = time.monotonic()
        sent = 0

        while True:
            # Unacknowledged writes (fast); the launcher asks for a rewind if any get lost.
            while sent < len(payload):
                if state["rewind"] is not None:
                    if state["rewind"] < sent:
                        sent = state["rewind"]
                        state["rewinds"] += 1
                    state["rewind"] = None
                # Flow control: wait for the launcher to catch up. A rewind request ends
                # the wait; so does silence (1.5 s): then resend from the last confirmed byte.
                while sent - state["received"] > window and state["rewind"] is None:
                    try:
                        await wait_change(1.5)
                    except asyncio.TimeoutError:
                        state["rewind"] = state["received"]
                if state["rewind"] is not None:
                    continue
                while not write_queue_ready(client):
                    await asyncio.sleep(0.002)
                piece = payload[sent:sent + chunk]
                await client.write_gatt_char(DATA, struct.pack("<I", sent) + piece, response=False)
                sent += len(piece)
                print(f"\r  {sent * 100 // len(payload):3d}%  ({state['rewinds']} rewinds)", end="",
                      file=sys.stderr)

            reply = await request({"cmd": "install.end"}, timeout=40)
            if reply.get("ok"):
                break
            if "missingFrom" in reply:  # the last writes were lost: send the tail again
                sent = reply["missingFrom"]
                state["rewinds"] += 1
                continue
            sys.exit(f"\ninstall failed: {reply.get('error')}")

        elapsed = time.monotonic() - start
        print(f"\ninstalled in {elapsed:.1f} s ({len(image) / 1024 / elapsed:.1f} KB/s, "
              f"{chunk} B chunks, {state['rewinds']} rewinds); {meta['name']} is starting",
              file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", help="device name, e.g. Dotty-SP01 (default: first found)")
    parser.add_argument("--timeout", type=float, default=6, help="scan time in seconds")
    parser.add_argument("--catalog", default=CATALOG_URL, help="catalog.json URL")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("scan")
    sub.add_parser("info")
    sub.add_parser("listen")
    cmd = sub.add_parser("cmd", help="send a command: NAME [key=value | key:=json ...] or a JSON object")
    cmd.add_argument("cmd")
    cmd.add_argument("params", nargs="*")
    cmd.add_argument("--wait", type=float, default=30, help="seconds to wait for the reply")
    sub.add_parser("catalog", help="list the published cartridges")
    inst = sub.add_parser("install", help="install a cartridge (.bin file or catalog id) through the launcher")
    inst.add_argument("firmware", help="path to a firmware .bin, or a cartridge id from the catalog")
    inst.add_argument("--icon", help="icon image (default: cartridges/<id>/icon.png)")
    inst.add_argument("--corrupt", action="store_true", help="test: flip one byte after hashing")
    args = parser.parse_args()
    actions = {"scan": scan, "info": info, "listen": listen, "cmd": command, "install": install,
               "catalog": catalog}
    asyncio.run(actions[args.action](args))


if __name__ == "__main__":
    main()
