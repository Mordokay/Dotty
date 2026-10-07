#!/usr/bin/env python3
"""Draws the pet cartridge's 1-bit art and writes cartridges/pet/images/sprites.h.

  python3 tools/pet_art.py            # header + preview sheet (pet_art_preview.png)

Creatures are 32x32 (drawn x3 on Dotty = 96 px, x2 on the lock screen), built from simple
shapes (a body, then ears, sprouts, feet…) plus one shared face kit, so every species gets
every pose: idle, bob (the second idle frame), blink, happy, sad (sick), angry (scolded),
eat (mouth open), no (refusing: looks away). Items and icons are 16x16 ASCII grids.
Set bit = black, rows MSB-first: what Adafruit_GFX::drawBitmap expects.
"""

from pathlib import Path

from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "cartridges/pet/images/sprites.h"
PREVIEW = Path(__file__).resolve().parent / "pet_art_preview.png"

N = 32  # creature size
POSES = ["idle", "bob", "blink", "happy", "sad", "angry", "eat", "no"]


# ---------- the face kit ----------

def face(d, cx, cy, gap, pose, big=False, mouth=True):
    """Eyes `gap` px either side of cx, mouth below. d draws black (0) on white (1)."""
    ex = [cx - gap, cx + gap]
    if pose == "no":
        ex = [x - 2 for x in ex]  # looking away
    eh = 3 if big else 2
    for x in ex:
        if pose in ("blink",):
            d.line([(x - 1, cy + 1), (x + 1, cy + 1)], fill=0)
        elif pose == "happy":  # ^ ^
            d.point([(x - 1, cy + 1), (x, cy), (x + 1, cy + 1)], fill=0)
        else:
            d.rectangle([x - (1 if big else 0), cy - (1 if big else 0), x + 1, cy - 1 + eh], fill=0)
            if big:
                d.point([(x, cy - 1)], fill=1)  # a glint
    if pose == "sad":  # brows down at the sides
        for x, s in ((ex[0], -1), (ex[1], 1)):
            d.line([(x - s, cy - 3), (x + 2 * s, cy - 2)], fill=0)
    if pose == "angry":  # brows down in the middle
        for x, s in ((ex[0], 1), (ex[1], -1)):
            d.line([(x - s, cy - 3), (x + s, cy - 2)], fill=0)
    if not mouth:
        return
    my = cy + (5 if big else 4)
    if pose == "eat":
        d.ellipse([cx - 2, my - 1, cx + 2, my + 3], outline=0, fill=1)
    elif pose == "happy":
        d.chord([cx - 3, my - 2, cx + 3, my + 3], 0, 180, fill=0)
    elif pose in ("sad", "angry", "no"):
        d.line([(cx - 2, my + 1), (cx - 1, my), (cx + 1, my), (cx + 2, my + 1)] if pose == "sad"
               else [(cx - 2, my), (cx + 2, my)], fill=0)
    else:  # a small smile
        d.line([(cx - 2, my), (cx - 1, my + 1), (cx + 1, my + 1), (cx + 2, my)], fill=0)


def blob(d, box, width=1):
    d.ellipse(box, outline=0, fill=1, width=width)


# ---------- the species (P1 order: baby, child, teen A, teen B, 6 adults, secret) ----------

def baby(d, pose):
    blob(d, [9, 17, 23, 31])
    face(d, 16, 23, 3, pose)


def child(d, pose):
    for x in (10, 19):  # little feet
        d.rectangle([x, 29, x + 3, 31], fill=0)
    blob(d, [6, 12, 26, 30])
    face(d, 16, 20, 4, pose)


def teen_a(d, pose):  # good care: a round one with a sprout
    d.line([(16, 11), (16, 5)], fill=0)
    d.ellipse([9, 2, 16, 7], outline=0, fill=1)
    d.ellipse([16, 1, 23, 6], outline=0, fill=1)
    for x in (9, 20):
        d.rectangle([x, 29, x + 3, 31], fill=0)
    blob(d, [5, 10, 27, 30])
    face(d, 16, 19, 5, pose)


def teen_b(d, pose):  # poor care: lumpy, with a bent antenna
    d.line([(20, 13), (23, 6), (27, 7)], fill=0)
    d.ellipse([26, 5, 29, 8], fill=0)
    blob(d, [2, 17, 12, 27])
    blob(d, [5, 12, 29, 31])
    d.line([(7, 18), (7, 25)], fill=1)  # merge the bump into the body
    face(d, 18, 21, 5, pose)


def adult1(d, pose):  # the best: cat-like, tall ears, a curled tail
    d.arc([22, 18, 31, 28], 270, 120, fill=0, width=2)
    d.polygon([(7, 13), (9, 2), (15, 10)], outline=0, fill=1)
    d.polygon([(25, 13), (23, 2), (17, 10)], outline=0, fill=1)
    blob(d, [5, 8, 27, 31])
    d.line([(9, 11), (14, 9)], fill=1)
    d.line([(23, 11), (18, 9)], fill=1)
    face(d, 16, 17, 6, pose, big=True)


def adult2(d, pose):  # round bear ears, a belly
    blob(d, [3, 4, 11, 12])
    blob(d, [21, 4, 29, 12])
    blob(d, [3, 7, 29, 31])
    face(d, 16, 15, 6, pose, big=True)


def adult3(d, pose):  # a crest and a mask
    d.polygon([(11, 9), (13, 1), (16, 8), (19, 1), (21, 9)], outline=0, fill=1)
    blob(d, [5, 7, 27, 31])
    d.rectangle([7, 13, 25, 19], fill=0)  # the mask
    for x in (11, 21):  # eyes in the mask (white)
        if pose == "blink":
            d.line([(x - 1, 17), (x + 1, 17)], fill=1)
        elif pose == "happy":
            d.point([(x - 1, 17), (x, 16), (x + 1, 17)], fill=1)
        else:
            dx = -2 if pose == "no" else 0
            d.rectangle([x - 1 + dx, 15, x + 1 + dx, 17], fill=1)
    face(d, 16, 19, 5, pose, mouth=True) if False else None
    my = 23
    if pose == "eat":
        d.ellipse([14, my - 1, 18, my + 3], outline=0, fill=1)
    elif pose == "happy":
        d.chord([13, my - 2, 19, my + 3], 0, 180, fill=0)
    elif pose in ("sad", "angry", "no"):
        d.line([(14, my), (18, my)], fill=0)
    else:
        d.line([(14, my), (15, my + 1), (17, my + 1), (18, my)], fill=0)
    if pose in ("sad", "angry"):
        d.line([(9, 11), (13, 12)] if pose == "angry" else [(9, 12), (13, 11)], fill=0)
        d.line([(23, 11), (19, 12)] if pose == "angry" else [(23, 12), (19, 11)], fill=0)


def adult4(d, pose):  # all mouth
    for x in (7, 22):
        d.rectangle([x, 29, x + 3, 31], fill=0)
    blob(d, [2, 9, 30, 30])
    cx, cy = 16, 15
    face(d, cx, cy, 7, pose, big=True, mouth=False)
    if pose == "eat":
        d.ellipse([10, 19, 22, 27], fill=0)
    elif pose in ("sad", "angry", "no"):
        d.line([(10, 22), (22, 22)], fill=0)
    else:  # a wide grin with teeth
        d.chord([8, 16, 24, 27], 0, 180, fill=0)
        for x in (11, 15, 19):
            d.rectangle([x, 22, x + 1, 23], fill=1)


def adult5(d, pose):  # a worm, standing up
    blob(d, [2, 22, 20, 31])
    blob(d, [8, 13, 24, 26])
    blob(d, [12, 2, 28, 18])
    d.line([(10, 24), (16, 24)], fill=1)
    d.line([(15, 16), (21, 16)], fill=1)
    face(d, 20, 8, 3, pose)


def adult6(d, pose):  # spiky
    pts = []
    import math
    for i in range(16):
        r = 14 if i % 2 == 0 else 10
        a = math.pi * 2 * i / 16 - math.pi / 2
        pts.append((16 + r * math.cos(a), 18 + r * math.sin(a)))
    d.polygon(pts, outline=0, fill=1)
    d.ellipse([7, 9, 25, 27], fill=1)
    face(d, 16, 17, 4, pose if pose != "idle" else "angry" if False else pose)
    if pose in ("idle", "bob"):  # always a bit grumpy
        d.line([(10, 13), (13, 14)], fill=0)
        d.line([(22, 13), (19, 14)], fill=0)


def secret(d, pose):  # an old gentleman: a top hat and a moustache
    blob(d, [4, 10, 28, 31])
    d.rectangle([9, 1, 23, 9], fill=0)
    d.rectangle([5, 9, 27, 11], fill=0)
    face(d, 16, 17, 5, pose, mouth=False)
    my = 23
    d.chord([9, 20, 16, 25], 180, 360, fill=0)
    d.chord([16, 20, 23, 25], 180, 360, fill=0)
    if pose == "eat":
        d.ellipse([14, my, 18, my + 4], outline=0, fill=1)
    elif pose == "happy":
        d.line([(13, my + 2), (16, my + 3), (19, my + 2)], fill=0)


SPECIES = [("baby", baby), ("child", child), ("teenA", teen_a), ("teenB", teen_b),
           ("adult1", adult1), ("adult2", adult2), ("adult3", adult3), ("adult4", adult4),
           ("adult5", adult5), ("adult6", adult6), ("secret", secret)]


def creature(draw_fn, pose):
    img = Image.new("1", (N, N), 1)
    draw_fn(ImageDraw.Draw(img), "idle" if pose == "bob" else pose)
    if pose == "bob":  # the second idle frame: squashed down a pixel
        squashed = img.crop((0, 0, N, N - 1)).resize((N, N - 2), Image.NEAREST)
        img = Image.new("1", (N, N), 1)
        img.paste(squashed, (0, 2))
    return img


# ---------- other 32x32 pictures ----------

def egg(frame):
    img = Image.new("1", (N, N), 1)
    d = ImageDraw.Draw(img)
    dx = [0, 1, 0][frame]
    d.ellipse([8 + dx, 3, 24 + dx, 31], outline=0, fill=1)
    d.line([(9 + dx, 17), (12 + dx, 14), (16 + dx, 18), (20 + dx, 14), (23 + dx, 17)], fill=0)
    for x, y in ((13, 8), (19, 10), (12, 24), (19, 25)):
        d.ellipse([x + dx, y, x + 2 + dx, y + 2], fill=0)
    if frame == 2:  # cracking
        d.line([(16, 3), (14, 7), (18, 10), (15, 13)], fill=0)
    return img


def angel():
    img = Image.new("1", (N, N), 1)
    d = ImageDraw.Draw(img)
    d.ellipse([10, 1, 22, 5], outline=0)  # halo
    d.polygon([(4, 14), (9, 10), (9, 20)], outline=0, fill=1)  # wings
    d.polygon([(28, 14), (23, 10), (23, 20)], outline=0, fill=1)
    d.pieslice([8, 7, 24, 23], 180, 360, outline=0, fill=1)
    d.rectangle([8, 15, 24, 27], fill=1)
    d.line([(8, 15), (8, 28)], fill=0)
    d.line([(24, 15), (24, 28)], fill=0)
    d.line([(8, 28), (11, 25), (14, 28), (17, 25), (20, 28), (24, 25)], fill=0)
    face(d, 16, 15, 4, "blink")
    return img


# ---------- 16x16 items and icons (ASCII: # = black) ----------

ICONS = {
    "meal": """
................
......####......
.....#....#.....
....#......#....
...#........#...
...#........#...
..#..........#..
..#..........#..
.#............#.
.#..########..#.
.#..########..#.
#...########...#
#...########...#
#..............#
.##############.
................""",
    "snack": """
................
................
.##..........##.
#..#..####..#..#
#...##....##...#
#...#..#...#...#
#..#..#.#...#..#
.##..#...#..##..
.##...#...#.##..
#..#...#.#..#..#
#...#...#..#...#
#...##....##...#
#..#..####..#..#
.##..........##.
................
................""",
    "poop": """
................
................
.......#........
......#.#.......
.......##.......
......#..#......
.....#....#.....
....#..##..#....
....#.#..#.#....
...#........#...
..#..######..#..
.#..#......#..#.
.#............#.
..############..
................
................""",
    "poop2": """
.....#....#.....
....#....#......
.....#....#.....
.......#........
......#.#.......
.......##.......
......#..#......
.....#....#.....
....#..##..#....
....#.#..#.#....
...#........#...
..#..######..#..
.#..#......#..#.
.#............#.
..############..
................""",
    "skull": """
................
.....######.....
...##......##...
..#..........#..
.#............#.
.#..###..###..#.
.#..###..###..#.
.#..###..###..#.
..#.....#....#..
...#..##.##.#...
...##.......#...
....#.#.#.#.#...
....#########...
................
................
................""",
    "syringe": """
................
.............#..
............#.#.
...........#.#..
..........###...
.........#..#...
........#..#....
.......#..#.....
......#..#......
.....#..#.......
....#..#........
...####.........
..##............
.#..............
#...............
................""",
    "duck": """
................
.......####.....
......#....#....
......#..#.#....
......#....###..
.......#..#.....
.......#..#.....
..##..#....#....
..#.##......#...
..#..........#..
..#..........#..
...#........#...
....########....
................
~~~~~~~~~~~~~~~~
................""",
    "zzz": """
................
.........######.
............#...
...........#....
..........#.....
.........######.
................
..#####.........
.....#..........
....#...........
...#............
..#####.........
................
................
................
................""",
    "bulb": """
.....######.....
....#......#....
...#........#...
..#..........#..
..#...#..#...#..
..#....##....#..
..#....##....#..
...#...##...#...
....#..##..#....
....#..##..#....
.....######.....
.....######.....
.....#....#.....
.....######.....
......####......
................""",
    "heart": """
................
..####...####...
.######.######..
################
################
################
################
.##############.
..############..
...##########...
....########....
.....######.....
......####......
.......##.......
................
................""",
    "heart_empty": """
................
..####...####...
.#....#.#....#..
#......#......#.
#.............#.
#.............#.
#.............#.
.#...........#..
..#.........#...
...#.......#....
....#.....#.....
.....#...#......
......#.#.......
.......#........
................
................""",
    "attention": """
................
......####......
.....######.....
.....######.....
.....######.....
.....######.....
.....######.....
......####......
......####......
......####......
................
......####......
.....######.....
......####......
................
................""",
    "pause": """
................
................
...####..####...
...####..####...
...####..####...
...####..####...
...####..####...
...####..####...
...####..####...
...####..####...
...####..####...
...####..####...
...####..####...
................
................
................""",
    "food": """
................
..#.#.#.....##..
..#.#.#....###..
..#.#.#...####..
..#.#.#...####..
..#####...####..
...###....####..
....#.....####..
....#......##...
....#......##...
....#......##...
....#......##...
....#......##...
....#......##...
....#......##...
................""",
    "game": """
................
.....######.....
...##..##..##...
..#...####...#..
.#...######...#.
.#..########..#.
#...########...#
#....######....#
#....######....#
#...########...#
.#..########..#.
.#...######...#.
..#...####...#..
...##..##..##...
.....######.....
................""",
    "meter": """
................
................
.............##.
.............##.
.........##..##.
.........##..##.
.....##..##..##.
.....##..##..##.
.##..##..##..##.
.##..##..##..##.
.##..##..##..##.
.##..##..##..##.
.##..##..##..##.
################
................
................""",
    "discipline": """
................
.......##.......
......####......
......#..#......
.....##..##.....
.....#....#.....
....##.##.##....
....#..##..#....
...##..##..##...
...#...##...#...
..##........##..
..#....##....#..
.##....##....##.
.#............#.
.##############.
................""",
    "bell": """
................
.......##.......
.....######.....
....#......#....
...#........#...
...#........#...
...#........#...
...#........#...
..#..........#..
..#..........#..
.#............#.
.##############.
................
......####......
.......##.......
................""",
}


def icon(name):
    rows = [r for r in ICONS[name].strip("\n").split("\n")]
    img = Image.new("1", (16, 16), 1)
    for y, row in enumerate(rows[:16]):
        for x, ch in enumerate(row[:16]):
            if ch == "#":
                img.putpixel((x, y), 0)
    return img


# ---------- output ----------

def to_bytes(img):
    w, h = img.size
    out = []
    for y in range(h):
        for bx in range(0, w, 8):
            b = 0
            for i in range(8):
                x = bx + i
                if x < w and img.getpixel((x, y)) == 0:
                    b |= 0x80 >> i
            out.append(b)
    return out


def c_array(data):
    return ", ".join(f"0x{b:02X}" for b in data)


def main():
    lines = ["// Generated by tools/pet_art.py — edit that, not this.", "#pragma once", "",
             "#include <Arduino.h>", "", "namespace art {", "",
             f"constexpr int kPetSize = {N};  // creatures: {N}x{N}, rows MSB-first, set bit = black",
             "enum Pose : uint8_t { " + ", ".join(f"k{p.capitalize()}" for p in POSES) + ", kPoseCount };", ""]
    lines.append(f"// [species: P1 order baby…secret][pose]")
    lines.append(f"const uint8_t kPets[{len(SPECIES)}][{len(POSES)}][{N * N // 8}] PROGMEM = {{")
    sheet = Image.new("1", (N * 3 * len(POSES) + 20, (N * 3 + 10) * (len(SPECIES) + 2)), 1)
    for si, (name, fn) in enumerate(SPECIES):
        lines.append(f"  {{  // {name}")
        for pi, pose in enumerate(POSES):
            img = creature(fn, pose)
            lines.append(f"    {{{c_array(to_bytes(img))}}},  // {pose}")
            sheet.paste(img.resize((N * 3, N * 3), Image.NEAREST), (10 + pi * N * 3, 5 + si * (N * 3 + 10)))
        lines.append("  },")
    lines.append("};")
    extra = [("kEgg", [egg(0), egg(1), egg(2)]), ("kAngel", [angel()])]
    row, col = len(SPECIES), 0
    for name, imgs in extra:
        lines.append(f"const uint8_t {name}[{len(imgs)}][{N * N // 8}] PROGMEM = {{")
        for img in imgs:
            lines.append(f"  {{{c_array(to_bytes(img))}}},")
            sheet.paste(img.resize((N * 3, N * 3), Image.NEAREST), (10 + col * N * 3, 5 + row * (N * 3 + 10)))
            col += 1
        lines.append("};")
    names = list(ICONS)
    lines.append("")
    lines.append("enum Icon : uint8_t { " + ", ".join("k" + "".join(p.capitalize() for p in n.split("_")) + "Icon" for n in names) + ", kIconCount };")
    lines.append(f"const uint8_t kIcons[{len(names)}][32] PROGMEM = {{")
    for i, n in enumerate(names):
        img = icon(n)
        lines.append(f"  {{{c_array(to_bytes(img))}}},  // {n}")
        sheet.paste(img.resize((32, 32), Image.NEAREST), (10 + (i % 18) * 42, 5 + (len(SPECIES) + 1) * (N * 3 + 10) + (i // 18) * 42))
    lines.append("};")
    lines += ["", "}  // namespace art", ""]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines))
    sheet.save(PREVIEW)
    print(f"wrote {OUT.relative_to(REPO)} and {PREVIEW.relative_to(REPO)}")


if __name__ == "__main__":
    main()
