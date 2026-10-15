#!/usr/bin/env python3
"""Builds cartridges/facts/facts.txt (the Facts cartridge's 10,000 facts, embedded in its
firmware) from one text file per category: a fact per line.

    python3 tools/build_facts.py SOURCE_DIR

Checks every line (ASCII, 40-200 characters, ends like a sentence), drops duplicates and near
duplicates (same words ignoring case and punctuation, across categories too), and writes
"@key|Name" before each category's facts. Prints what it dropped and the counts.
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "cartridges" / "facts" / "facts.txt"

# Menu order: key (the source file's name), name on Dotty.
CATEGORIES = [
    ("space", "Space"),
    ("animals", "Animals"),
    ("body", "Human body"),
    ("history", "History"),
    ("sea", "Sea life"),
    ("mind", "Mind"),
    ("earth", "Earth"),
    ("tech", "Technology"),
    ("ancient", "Ancient world"),
    ("bugs", "Bugs"),
    ("physics", "Physics"),
    ("art", "Art & music"),
    ("prehistoric", "Prehistoric"),
    ("plants", "Plants"),
    ("words", "Words"),
    ("world", "World"),
    ("chemistry", "Chemistry"),
    ("inventions", "Inventions"),
    ("microbes", "Microbes"),
    ("math", "Math"),
]
MIN_LEN, MAX_LEN = 40, 200


def key(text):
    """For spotting near duplicates: lower case words only."""
    return " ".join(re.findall(r"[a-z0-9]+", text.lower()))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    source = Path(sys.argv[1])
    seen = {}
    out = []
    total = 0
    for cat, name in CATEGORIES:
        path = source / f"{cat}.txt"
        if not path.exists():
            sys.exit(f"missing {path}")
        kept = 0
        out.append(f"@{cat}|{name}")
        for n, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            line = " ".join(raw.replace("\t", " ").split())
            if not line:
                continue
            why = None
            if any(ord(c) < 32 or ord(c) > 126 for c in line):
                why = "not ASCII"
            elif not MIN_LEN <= len(line) <= MAX_LEN:
                why = f"{len(line)} characters"
            elif line[-1] not in ".!?\")'":
                why = "doesn't end like a sentence"
            elif line.startswith("@"):
                why = "starts with @"
            elif key(line) in seen:
                why = f"duplicate of {seen[key(line)]}"
            if why:
                print(f"  dropped {cat}:{n} ({why}): {line[:70]}")
                continue
            seen[key(line)] = f"{cat}:{n}"
            out.append(line)
            kept += 1
        print(f"{cat:12} {kept}")
        total += kept
    OUT.write_text("\n".join(out) + "\n", encoding="ascii")
    print(f"{total} facts, {OUT.stat().st_size // 1024} KB -> {OUT.relative_to(REPO)}")


if __name__ == "__main__":
    main()
