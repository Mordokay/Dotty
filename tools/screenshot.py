#!/usr/bin/env python3
"""Saves Dotty's e-paper screen as a PNG (Dotty must be on USB, with a computer attached).

  ~/.platformio/penv/bin/python tools/screenshot.py [out.png]

Sends 's' over serial; the shell answers with the 200x200 1-bit frame buffer as hex
(1 = white, MSB first, 25 bytes per row). Needs Pillow for the PNG (falls back to a PBM).
"""
import glob
import sys
import time

import serial

port = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
out = sys.argv[1] if len(sys.argv) > 1 else "dotty-screen.png"
s = serial.Serial(port, 115200, timeout=0.5)
time.sleep(0.5)
s.read(100000)
s.write(b"s")
text, deadline = "", time.time() + 10
while "#END" not in text and time.time() < deadline:
    text += s.read(4096).decode(errors="replace")
if "#SCREEN" not in text or "#END" not in text:
    sys.exit("no screenshot came back (is Dotty unlocked and on USB?)")
body = text[text.index("#SCREEN"):text.index("#END")].splitlines()
w, h = map(int, body[0].split()[1:3])
data = bytes.fromhex("".join(line.strip() for line in body[1:] if line.strip()))
try:
    from PIL import Image
    img = Image.frombytes("1", (w, h), data)  # PIL mode "1" packs MSB first, 1 = white
    img.resize((w * 2, h * 2), Image.NEAREST).save(out)
except ImportError:
    out = out.rsplit(".", 1)[0] + ".pbm"
    with open(out, "wb") as f:  # PBM: 1 = black, so invert
        f.write(f"P4 {w} {h}\n".encode() + bytes(b ^ 0xFF for b in data))
print(out)
