#!/usr/bin/env python3
"""Talk to Dotty over BLE from the Mac, the same way the iOS app will (Dotty Core service).

Setup once:
  ~/.platformio/penv/bin/uv venv .venv --python ~/.platformio/penv/bin/python
  ~/.platformio/penv/bin/uv pip install --python .venv/bin/python bleak

Examples:
  .venv/bin/python tools/ble_dotty.py scan
  .venv/bin/python tools/ble_dotty.py info
  .venv/bin/python tools/ble_dotty.py cmd music.volume value=60
  .venv/bin/python tools/ble_dotty.py cmd core.toLauncher
  .venv/bin/python tools/ble_dotty.py listen

The first run asks macOS for Bluetooth permission for your terminal app.
"""

import argparse
import asyncio
import json
import sys

from bleak import BleakClient, BleakScanner

SERVICE = "b9c10000-fbaa-4525-8400-055f7a543231"
INFO = "b9c10001-fbaa-4525-8400-055f7a543231"
COMMAND = "b9c10002-fbaa-4525-8400-055f7a543231"
EVENT = "b9c10003-fbaa-4525-8400-055f7a543231"


async def find(name, timeout):
    devices = await BleakScanner.discover(timeout=timeout, service_uuids=[SERVICE], return_adv=True)
    found = [(d, adv) for d, adv in devices.values() if not name or (d.name or "").lower() == name.lower()]
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
        print(f"{device.name or '?':12} {device.address}  RSSI {adv.rssi} dBm")


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
    args = parser.parse_args()
    asyncio.run({"scan": scan, "info": info, "listen": listen, "cmd": command}[args.action](args))


if __name__ == "__main__":
    main()
