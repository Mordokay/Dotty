#!/usr/bin/env python3
"""Talk to Dotty over BLE from the Mac, the same way the iOS app will (Dotty Core service).

Setup once:
  ~/.platformio/penv/bin/uv venv .venv --python ~/.platformio/penv/bin/python
  ~/.platformio/penv/bin/uv pip install --python .venv/bin/python bleak pillow

Examples:
  .venv/bin/python tools/ble_dotty.py scan
  .venv/bin/python tools/ble_dotty.py info
  .venv/bin/python tools/ble_dotty.py cmd music.volume value=60
  .venv/bin/python tools/ble_dotty.py cmd core.toLauncher
  .venv/bin/python tools/ble_dotty.py listen
  .venv/bin/python tools/ble_dotty.py install .pio/build/music/firmware.bin

The first run asks macOS for Bluetooth permission for your terminal app.
"""

import argparse
import asyncio
import hashlib
import json
import struct
import sys
import time
from pathlib import Path

from bleak import BleakClient, BleakScanner

SERVICE = "b9c10000-fbaa-4525-8400-055f7a543231"
INFO = "b9c10001-fbaa-4525-8400-055f7a543231"
COMMAND = "b9c10002-fbaa-4525-8400-055f7a543231"
EVENT = "b9c10003-fbaa-4525-8400-055f7a543231"
DATA = "b9c10004-fbaa-4525-8400-055f7a543231"

REPO = Path(__file__).resolve().parent.parent
CARTRIDGE_INFO_OFFSET = 0x120  # DOTTY_CARTRIDGE() struct, right after esp_app_desc_t
CARTRIDGE_MAGIC = 0x59544F44
ICON_SIZE = 64


def serial_from(adv):
    """Chip serial from the manufacturer data (company 0xFFFF + 6 bytes)."""
    data = adv.manufacturer_data.get(0xFFFF)
    return ":".join(f"{b:02X}" for b in data) if data and len(data) == 6 else "?"


async def find(name, timeout):
    devices = await BleakScanner.discover(timeout=timeout, service_uuids=[SERVICE], return_adv=True)
    wanted = (name or "").lower()
    found = [(d, adv) for d, adv in devices.values()
             if not wanted or wanted in ((d.name or "").lower(), serial_from(adv).lower())]
    if not found:
        sys.exit("No Dotty found. Is it unlocked (Bluetooth is off while locked and asleep)?")
    return found


def parse_value(text):
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


def build_command(cmd, params):
    if cmd.startswith("{"):
        return json.loads(cmd)
    message = {"cmd": cmd}
    for param in params:
        key, _, value = param.partition("=")
        message[key] = parse_value(value)
    return message


async def scan(args):
    for device, adv in await find(args.name, args.timeout):
        print(f"{device.name or '?':12} serial {serial_from(adv)}  RSSI {adv.rssi} dBm")


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
        await client.start_notify(EVENT, on_event)
        await client.write_gatt_char(COMMAND, json.dumps(message).encode(), response=True)
        try:
            print(json.dumps(await asyncio.wait_for(reply, 5), indent=2))
        except asyncio.TimeoutError:
            print("no reply within 5 s (the device may have rebooted)", file=sys.stderr)


async def listen(args):
    async with await connect(args) as client:
        await client.start_notify(EVENT, lambda _, data: print(data.decode()))
        print("listening, Ctrl+C to stop", file=sys.stderr)
        while client.is_connected:
            await asyncio.sleep(1)


def write_queue_ready(client):
    """True when CoreBluetooth can take another unacknowledged write. bleak doesn't
    check this on macOS, and writes sent while the queue is full are silently dropped
    (the iOS app must check canSendWriteWithoutResponse the same way)."""
    peripheral = getattr(getattr(client, "_backend", None), "_peripheral", None)
    return peripheral is None or peripheral.canSendWriteWithoutResponse()


def read_cartridge_info(image):
    magic, = struct.unpack_from("<I", image, CARTRIDGE_INFO_OFFSET)
    if magic != CARTRIDGE_MAGIC:
        sys.exit("Not a Dotty cartridge image (no DOTTY_CARTRIDGE info at 0x120)")
    def text(offset, size):
        raw = image[CARTRIDGE_INFO_OFFSET + offset:CARTRIDGE_INFO_OFFSET + offset + size]
        return raw.split(b"\0")[0].decode()
    return {"id": text(4, 16), "name": text(20, 24), "version": text(44, 16)}


def icon_bytes(path):
    """64x64 1-bit, rows MSB-first, set bit = black (same layout as tools/img2epd.py)."""
    from PIL import Image
    img = Image.open(path).convert("L").resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS)
    out = bytearray(ICON_SIZE * ICON_SIZE // 8)
    for y in range(ICON_SIZE):
        for x in range(ICON_SIZE):
            if img.getpixel((x, y)) < 128:
                out[(y * ICON_SIZE + x) // 8] |= 0x80 >> (x % 8)
    return bytes(out)


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


async def install(args):
    image = Path(args.firmware).read_bytes()
    meta = read_cartridge_info(image)
    icon_path = Path(args.icon) if args.icon else REPO / "cartridges" / meta["id"] / "icon.png"
    icon = icon_bytes(icon_path) if icon_path.exists() else b""
    sha256 = hashlib.sha256(image).hexdigest()
    if args.corrupt:  # test the launcher's integrity check: one flipped byte
        image = image[:1000] + bytes([image[1000] ^ 0xFF]) + image[1001:]
    payload = icon + image  # the launcher splits the icon off the front
    print(f"{meta['name']} {meta['version']}: {len(image) // 1024} KB"
          f"{', icon ' + icon_path.name if icon else ', no icon'}", file=sys.stderr)

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
        await client.start_notify(EVENT, on_event)
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
    parser.add_argument("--name", help="device name, e.g. Dotty-B100 (default: first found)")
    parser.add_argument("--timeout", type=float, default=6, help="scan time in seconds")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("scan")
    sub.add_parser("info")
    sub.add_parser("listen")
    cmd = sub.add_parser("cmd", help="send a command: NAME [key=value ...] or a JSON object")
    cmd.add_argument("cmd")
    cmd.add_argument("params", nargs="*")
    inst = sub.add_parser("install", help="install a cartridge .bin through the launcher")
    inst.add_argument("firmware")
    inst.add_argument("--icon", help="icon image (default: cartridges/<id>/icon.png)")
    inst.add_argument("--corrupt", action="store_true", help="test: flip one byte after hashing")
    args = parser.parse_args()
    actions = {"scan": scan, "info": info, "listen": listen, "cmd": command, "install": install}
    asyncio.run(actions[args.action](args))


if __name__ == "__main__":
    main()
