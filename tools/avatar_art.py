#!/usr/bin/env python3
"""The companion's ten avatars → cartridges/companion/images/avatars.h (1-bit, 96x96 and 48x48,
every expression), tools/avatar_art_preview.png (check by eye) and the app's previews
(ios/Dotty/Resources/Avatars/<id>.png).

An avatar's body is the user's picture `tools/avatar_art_src/<id>.png` when there is one (made
with docs/companion-avatar-prompts.md: black lines on white, face left empty), else a simple
placeholder drawn here. The face (eyes, eyebrows, mouth) is drawn by this script in a Disney-ish
style: big glossy eyes with two highlights, eyebrows that carry the emotion, small mouths. Its
place and size per avatar are in FACE, tuned per picture.

    python3 tools/avatar_art.py        # system python3 has Pillow
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

REPO = Path(__file__).resolve().parent.parent
SRC = Path(__file__).resolve().parent / "avatar_art_src"
OUT = REPO / "cartridges/companion/images/avatars.h"
PREVIEW = Path(__file__).resolve().parent / "avatar_art_preview.png"
APP = REPO / "ios/Dotty/Resources/Avatars"

BIG, SMALL = 96, 48
IDS = ["robot", "dog", "cat", "fox", "owl", "bear", "girl", "guy", "ghost", "dragon"]
NAMES = ["Robot", "Dog", "Cat", "Fox", "Owl", "Bear", "Girl", "Guy", "Ghost", "Dragon"]
# Same order as avatar::Face in avatar.h.
FACES = ["idle", "blink", "listening", "thinking", "talkA", "talkB", "happy", "sad", "surprised",
         "laughing", "love", "sleeping"]

# Where the face goes, in 96-px coordinates: (centre x, eye line y, scale). Scale 1 = eyes 10 px
# apart from the centre. Tuned per picture; these fit the placeholders.
FACE = {
    "robot": (48, 46, 1.0), "dog": (48, 44, 0.95), "cat": (48, 46, 0.95), "fox": (48, 46, 0.9),
    "owl": (48, 38, 1.0), "bear": (48, 40, 0.95), "girl": (48, 40, 0.7), "guy": (48, 42, 0.7),
    "ghost": (48, 42, 1.0), "dragon": (48, 42, 0.85),
}
# Mouths go further down below a snout, nose or beak (96-px units at scale 1).
MOUTH_DROP = {"dog": 6, "cat": 3, "fox": 5, "owl": 11, "bear": 6, "girl": 2, "guy": 4, "dragon": 6}
# Marks in a picture where the face goes, whitened first: (x, y, radius) in 96-px coordinates.
ERASE = {"ghost": [(48, 40, 4)]}

# ---------- placeholder bodies (drawn 4x larger, then shrunk: smooth curves) ----------

K = 4  # supersampling
W = 9 * K // 2  # outline width at 4x (≈ 2 px at 96)


def ell(d, cx, cy, rx, ry, fill=255, width=W):
    d.ellipse([(cx - rx) * K, (cy - ry) * K, (cx + rx) * K, (cy + ry) * K], fill=fill, outline=0, width=width)


def poly(d, pts, fill=255, width=W):
    p = [(x * K, y * K) for x, y in pts]
    d.polygon(p, fill=fill)
    d.line(p + [p[0]], fill=0, width=width, joint="curve")


def shoulders(d, width=34):
    d.rounded_rectangle([(48 - width) * K, 82 * K, (48 + width) * K, 110 * K], radius=16 * K, fill=255, outline=0,
                        width=W)


def head(d, ry=30, rx=31, cy=50):
    ell(d, 48, cy, rx, ry)


def robot(d):
    shoulders(d, 30)
    d.line([48 * K, 18 * K, 48 * K, 8 * K], fill=0, width=W)
    ell(d, 48, 7, 4, 4, fill=0)
    for s in (-1, 1):
        ell(d, 48 + s * 36, 52, 5, 7)
    d.rounded_rectangle([14 * K, 18 * K, 82 * K, 82 * K], radius=16 * K, fill=255, outline=0, width=W)
    d.rounded_rectangle([22 * K, 30 * K, 74 * K, 72 * K], radius=10 * K, fill=255, outline=0, width=W // 2)
    for x in (20, 76):
        ell(d, x, 24, 1.5, 1.5, fill=0, width=1)


def dog(d):
    shoulders(d)
    head(d, 30, 30, 50)
    for s in (-1, 1):  # floppy ears, solid
        poly(d, [(48 + s * 22, 24), (48 + s * 38, 30), (48 + s * 40, 58), (48 + s * 30, 64), (48 + s * 26, 34)], fill=0)
    ell(d, 48, 61, 4, 3, fill=0)
    d.rounded_rectangle([36 * K, 80 * K, 60 * K, 85 * K], radius=2 * K, fill=0)
    ell(d, 48, 89, 4, 4)


def cat(d):
    shoulders(d)
    for s in (-1, 1):
        poly(d, [(48 + s * 12, 26), (48 + s * 30, 10), (48 + s * 30, 36)])
        poly(d, [(48 + s * 18, 24), (48 + s * 27, 16), (48 + s * 27, 30)], fill=0, width=1)
    head(d, 29, 32, 54)
    poly(d, [(45, 62), (51, 62), (48, 65)], fill=0, width=K)
    for s in (-1, 1):
        for dy in (-2, 3):
            d.line([(48 + s * 24) * K, (64 + dy) * K, (48 + s * 33) * K, (63 + dy * 1.6) * K], fill=0, width=K * 2)


def fox(d):
    shoulders(d)
    for s in (-1, 1):
        poly(d, [(48 + s * 10, 30), (48 + s * 26, 4), (48 + s * 32, 34)])
        poly(d, [(48 + s * 21, 12), (48 + s * 26, 4), (48 + s * 28, 16)], fill=0, width=1)
    poly(d, [(18, 44), (30, 26), (66, 26), (78, 44), (84, 64), (66, 72), (48, 80), (30, 72), (12, 64)])
    ell(d, 48, 66, 3.5, 2.5, fill=0)


def owl(d):
    shoulders(d, 36)
    for s in (-1, 1):
        poly(d, [(48 + s * 18, 22), (48 + s * 32, 8), (48 + s * 32, 26)])
    head(d, 32, 34, 50)
    for s in (-1, 1):
        ell(d, 48 + s * 12, 50, 13, 13, width=W // 2)
    poly(d, [(44, 58), (52, 58), (48, 66)], fill=0, width=K)
    for x in (36, 48, 60):
        d.arc([(x - 5) * K, 86 * K, (x + 5) * K, 96 * K], 0, 180, fill=0, width=K * 2)


def bear(d):
    shoulders(d)
    for s in (-1, 1):
        ell(d, 48 + s * 26, 24, 10, 10)
        ell(d, 48 + s * 26, 24, 5, 5, fill=0, width=1)
    head(d, 30, 32, 52)
    ell(d, 48, 66, 10, 8)
    ell(d, 48, 62, 4, 3, fill=0)
    d.rounded_rectangle([24 * K, 80 * K, 72 * K, 88 * K], radius=4 * K, fill=255, outline=0, width=W)


def girl(d):
    # Long blonde hair: white with a few strand lines, never solid.
    poly(d, [(16, 40), (22, 18), (48, 8), (74, 18), (80, 40), (84, 92), (12, 92)])
    shoulders(d, 28)
    head(d, 27, 25, 50)
    d.arc([20 * K, 16 * K, 80 * K, 60 * K], 200, 330, fill=0, width=W)  # fringe line
    poly(d, [(25, 36), (40, 22), (60, 18), (72, 30), (56, 28), (36, 34)])
    for x0, x1 in ((18, 16), (24, 22), (74, 78), (80, 82)):
        d.line([x0 * K, 54 * K, x1 * K, 88 * K], fill=0, width=K * 2)
    for a in range(5):  # a flower behind the right ear
        ang = a * 2 * math.pi / 5
        ell(d, 74 + 4 * math.cos(ang), 40 + 4 * math.sin(ang), 3, 3, width=K * 2)
    ell(d, 74, 40, 1.5, 1.5, fill=0, width=1)


def guy(d):
    shoulders(d, 32)
    poly(d, [(36, 82), (48, 96), (60, 82), (56, 100), (40, 100)])  # collar
    head(d, 28, 26, 52)
    poly(d, [(22, 46), (22, 28), (34, 16), (52, 12), (68, 18), (76, 32), (74, 46), (66, 32), (52, 28), (40, 34),
             (30, 32)], fill=0)
    d.line([40 * K, 18 * K, 56 * K, 17 * K], fill=255, width=K * 2)


def ghost(d):
    pts = [(14, 50)]
    for i in range(7):
        x = 14 + i * 68 / 6
        pts.append((x, 90 if i % 2 == 0 else 82))
    pts.append((82, 50))
    poly(d, pts[:-1] + [(82, 50)])
    ell(d, 48, 46, 34, 36)
    d.rectangle([15 * K, 46 * K, 81 * K, 82 * K], fill=255)
    for s in (-1, 1):
        d.line([(48 + s * 34) * K, 46 * K, (48 + s * 34) * K, 84 * K], fill=0, width=W)
        poly(d, [(48 + s * 33, 60), (48 + s * 44, 52), (48 + s * 40, 66)])


def dragon(d):
    shoulders(d)
    for s in (-1, 1):
        poly(d, [(48 + s * 30, 80), (48 + s * 46, 66), (48 + s * 44, 86)])  # wings
        poly(d, [(48 + s * 12, 24), (48 + s * 20, 6), (48 + s * 24, 26)], fill=0)  # horns
        poly(d, [(48 + s * 28, 40), (48 + s * 42, 36), (48 + s * 32, 54)])  # frills
    head(d, 29, 31, 50)
    ell(d, 48, 66, 12, 8)
    for s in (-1, 1):
        ell(d, 48 + s * 4, 64, 1.5, 1.5, fill=0, width=1)


PLACEHOLDERS = {"robot": robot, "dog": dog, "cat": cat, "fox": fox, "owl": owl, "bear": bear, "girl": girl,
                "guy": guy, "ghost": ghost, "dragon": dragon}


def body(key, size):
    """The avatar without a face, `size` px, black and white."""
    src = SRC / f"{key}.png"
    if src.exists():
        g = Image.open(src).convert("L")
        box = g.point(lambda v: 255 if v < 128 else 0).getbbox()
        if box:
            g = g.crop(box)
            side = max(g.width, g.height)
            sq = Image.new("L", (side, side), 255)
            sq.paste(g, ((side - g.width) // 2, side - g.height))  # shoulders on the bottom edge
            grow = max(3, int(side / size * 0.5)) | 1  # thicken so lines survive the shrink
            sq = sq.filter(ImageFilter.MinFilter(grow))
            return sq.resize((size, size), Image.LANCZOS).point(lambda v: 0 if v < 150 else 255).convert("1")
    big = Image.new("L", (BIG * K, BIG * K), 255)
    PLACEHOLDERS[key](ImageDraw.Draw(big))
    return big.resize((size, size), Image.LANCZOS).point(lambda v: 0 if v < 140 else 255).convert("1")


# ---------- the face kit (drawn at the final size: crisp pixels) ----------

def face(img, key, expr, size):
    d = ImageDraw.Draw(img)
    f = size / BIG
    cx, cy, sc = FACE[key]
    cx, cy, s = cx * f, cy * f, sc * f
    gap = 11 * s
    ew, eh = max(2, 5.5 * s), max(3, 7.5 * s)  # eye half-width / half-height: big and tall
    my = cy + (15 + MOUTH_DROP.get(key, 0)) * s
    lw = 2 if size >= 64 else 1

    def eye(x, open_=True, scale=1.0, look=(0, 0)):
        if not open_:
            return
        x += look[0] * s
        y = cy + look[1] * s
        w, h = ew * scale, eh * scale
        d.ellipse([x - w, y - h, x + w, y + h], fill=0)
        hr = max(1, round(w * 0.38))
        hx, hy = x + w * 0.25, y - h * 0.4  # the big highlight, then a small one
        d.ellipse([hx - hr, hy - hr, hx + hr, hy + hr], fill=1)
        if size >= 64:
            d.point([(round(x - w * 0.35), round(y + h * 0.45))], fill=1)

    def closed(x, happy=False):
        if happy:  # ^ : laughing / happy-closed
            d.arc([x - ew, cy - eh * 0.6, x + ew, cy + eh * 0.9], 200, 340, fill=0, width=lw)
        else:  # a soft downward lid line
            d.arc([x - ew, cy - eh * 0.9, x + ew, cy + eh * 0.5], 20, 160, fill=0, width=lw)

    def brows(kind):
        for side in (-1, 1):
            x = cx + side * gap
            y = cy - eh - 4 * s
            if kind == "flat":
                d.line([x - ew, y, x + ew, y], fill=0, width=lw)
            elif kind == "up":  # raised: listening, surprised
                d.arc([x - ew, y - 3 * s, x + ew, y + 3 * s], 200, 340, fill=0, width=lw)
            elif kind == "worried":  # inner ends up
                d.line([x - side * ew, y - 2.5 * s, x + side * ew, y + 1.5 * s], fill=0, width=lw)
            elif kind == "one":  # thinking: one raised
                if side > 0:
                    d.arc([x - ew, y - 3.5 * s, x + ew, y + 2.5 * s], 200, 340, fill=0, width=lw)
                else:
                    d.line([x - ew, y + 1, x + ew, y + 1], fill=0, width=lw)

    def smile(width=6, depth=3):
        d.arc([cx - width * s, my - depth * s, cx + width * s, my + depth * s], 20, 160, fill=0, width=lw)

    def frown():
        d.arc([cx - 5 * s, my, cx + 5 * s, my + 5 * s], 200, 340, fill=0, width=lw)

    def open_mouth(w, h):  # a grin: the lower half of an ellipse `h` tall
        d.chord([cx - w * s, my - h * s, cx + w * s, my + h * s], 0, 180, fill=0)

    def oval(w, h):  # an open "o", talking
        d.ellipse([cx - w * s, my - h * s / 2, cx + w * s, my + h * s / 2], fill=0)

    def heart(x):
        r = ew * 0.75
        d.ellipse([x - 2 * r, cy - r * 1.4, x, cy + r * 0.2], fill=0)
        d.ellipse([x, cy - r * 1.4, x + 2 * r, cy + r * 0.2], fill=0)
        d.polygon([(x - 2 * r, cy - r * 0.4), (x + 2 * r, cy - r * 0.4), (x, cy + r * 1.8)], fill=0)

    L, R = cx - gap, cx + gap
    if expr == "idle":
        eye(L), eye(R), smile()
    elif expr == "blink":
        closed(L), closed(R), smile()
    elif expr == "listening":
        brows("up"), eye(L), eye(R), d.ellipse([cx - 2 * s, my - 1 * s, cx + 2 * s, my + 3 * s], outline=0, width=lw)
    elif expr == "thinking":
        brows("one"), eye(L, look=(2, -2)), eye(R, look=(2, -2))
        d.line([cx - 1 * s, my + 1 * s, cx + 5 * s, my - 1 * s], fill=0, width=lw)
    elif expr == "talkA":
        eye(L), eye(R), oval(4, 7)
    elif expr == "talkB":
        eye(L), eye(R), oval(4.5, 3.5)
    elif expr == "happy":
        brows("up"), eye(L), eye(R), open_mouth(7, 6)
    elif expr == "sad":
        brows("worried"), eye(L, scale=0.9, look=(0, 1)), eye(R, scale=0.9, look=(0, 1)), frown()
    elif expr == "surprised":
        brows("up"), eye(L, scale=1.15), eye(R, scale=1.15)
        d.ellipse([cx - 3 * s, my - 2 * s, cx + 3 * s, my + 5 * s], fill=0)
    elif expr == "laughing":
        closed(L, happy=True), closed(R, happy=True), open_mouth(7, 8)
    elif expr == "love":
        heart(L), heart(R), smile(7, 4)
    elif expr == "sleeping":
        for x in (L, R):
            d.line([x - ew, cy + 1, x + ew, cy + 1], fill=0, width=lw)
        d.ellipse([cx - 1.5 * s, my, cx + 1.5 * s, my + 2.5 * s], outline=0, width=1)
    return img


def frame(key, expr, size):
    img = body(key, size)
    f = size / BIG
    d = ImageDraw.Draw(img)
    for x, y, r in ERASE.get(key, []) if (SRC / f"{key}.png").exists() else []:
        d.ellipse([(x - r) * f, (y - r) * f, (x + r) * f, (y + r) * f], fill=1)
    return face(img, key, expr, size)


# ---------- output ----------

def to_bytes(img):
    w, h = img.size
    out = []
    for y in range(h):
        for bx in range(0, w, 8):
            b = 0
            for i in range(8):
                if bx + i < w and img.getpixel((bx + i, y)) == 0:
                    b |= 0x80 >> i
            out.append(b)
    return out


def c_array(data):
    return ", ".join(f"0x{b:02X}" for b in data)


def main():
    lines = ["// Generated by tools/avatar_art.py — edit that, not this.", "#pragma once", "", "#include <Arduino.h>", "",
             "namespace art {", "",
             f"constexpr int kAvatarCount = {len(IDS)}, kFaceCount = {len(FACES)};",
             f"constexpr int kBigSize = {BIG}, kSmallSize = {SMALL};  // rows MSB-first, set bit = black",
             "const char *const kAvatarIds[kAvatarCount] = {" + ", ".join(f'"{i}"' for i in IDS) + "};",
             "const char *const kAvatarNames[kAvatarCount] = {" + ", ".join(f'"{n}"' for n in NAMES) + "};", ""]
    sheet = Image.new("1", (len(FACES) * (BIG + 6) + 6, len(IDS) * (BIG + SMALL + 14) + 6), 1)
    for size, name in ((BIG, "kBig"), (SMALL, "kSmall")):
        lines.append(f"// [avatar][face] (faces in avatar::Face order: {', '.join(FACES)})")
        lines.append(f"const uint8_t {name}[kAvatarCount][kFaceCount][{size * size // 8}] PROGMEM = {{")
        for ai, key in enumerate(IDS):
            lines.append(f"  {{  // {key}")
            for fi, expr in enumerate(FACES):
                img = frame(key, expr, size)
                lines.append(f"    {{{c_array(to_bytes(img))}}},  // {expr}")
                y = 6 + ai * (BIG + SMALL + 14) + (0 if size == BIG else BIG + 4)
                sheet.paste(img, (6 + fi * (BIG + 6), y))
            lines.append("  },")
        lines.append("};")
        lines.append("")
    lines.append("}  // namespace art")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines) + "\n")
    sheet.convert("L").resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(PREVIEW)
    APP.mkdir(parents=True, exist_ok=True)
    for key in IDS:  # the app's picture: the idle face, 2 screen pixels per dot
        frame(key, "idle", BIG).convert("L").resize((BIG * 2, BIG * 2), Image.NEAREST).save(APP / f"{key}.png")
    # The cartridge's install icon (64x64, 1-bit): the girl, happy (the app artwork is her too).
    frame("girl", "happy", 64).save(REPO / "cartridges/companion/icon.png")
    print(f"{len(IDS)} avatars x {len(FACES)} faces -> {OUT.relative_to(REPO)}, {PREVIEW.name}, {APP.relative_to(REPO)}/")


if __name__ == "__main__":
    main()
