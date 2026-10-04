"""Helpers shared by the Dotty tools: cartridge identity and icons.

A cartridge image carries its DOTTY_CARTRIDGE() struct (lib/dotty_core/src/cartridge.h)
at offset 0x120, right after ESP-IDF's app description:
  uint32 magic "DOTY", char id[16], char name[24], char version[16]
Icons are 64x64, 1 bit per pixel, rows MSB-first, set bit = black (512 bytes), the same
layout as tools/img2epd.py and the launcher's install screen.
"""

import struct
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CARTRIDGE_INFO_OFFSET = 0x120
CARTRIDGE_MAGIC = 0x59544F44
ICON_SIZE = 64


class NotACartridge(ValueError):
    pass


def read_cartridge_info(image: bytes) -> dict:
    """{"id", "name", "version"} from a firmware image."""
    if len(image) < CARTRIDGE_INFO_OFFSET + 60:
        raise NotACartridge("image too small")
    magic, = struct.unpack_from("<I", image, CARTRIDGE_INFO_OFFSET)
    if magic != CARTRIDGE_MAGIC:
        raise NotACartridge("no DOTTY_CARTRIDGE info at 0x120")

    def text(offset, size):
        start = CARTRIDGE_INFO_OFFSET + offset
        return image[start:start + size].split(b"\0")[0].decode()

    return {"id": text(4, 16), "name": text(20, 24), "version": text(44, 16)}


def icon_bytes(path) -> bytes:
    """Converts an icon image to the 512-byte 64x64 1-bit layout."""
    from PIL import Image

    img = Image.open(path).convert("L").resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS)
    out = bytearray(ICON_SIZE * ICON_SIZE // 8)
    for y in range(ICON_SIZE):
        for x in range(ICON_SIZE):
            if img.getpixel((x, y)) < 128:
                out[(y * ICON_SIZE + x) // 8] |= 0x80 >> (x % 8)
    return bytes(out)


def cartridge_icon_path(cartridge_id: str) -> Path:
    return REPO / "cartridges" / cartridge_id / "icon.png"
