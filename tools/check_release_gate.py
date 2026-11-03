#!/usr/bin/env python3
"""Checks that the Companion's DOTTY_RELEASE-gated "Developer aids" block holds only dev keys: lines that
every build needs (the note card's expiry once slipped inside, so release builds never cleared a note).

    python3 tools/check_release_gate.py      # exit 1 when something that isn't a dev key is gated
"""
import re
import sys
from pathlib import Path

MAIN = Path(__file__).resolve().parent.parent / "cartridges/companion/main.cpp"
# Things the release build must keep running; none may sit inside the gated block.
REQUIRED = [r"millis\(\) > noteUntil", r"The avatar's animation clock"]


def main():
    text = MAIN.read_text()
    start = text.index("#if !defined(DOTTY_RELEASE)", text.index("// Developer aids"))
    end = text.index("#endif", start)
    gated = text[start:end]
    bad = [r for r in REQUIRED if re.search(r, gated)]
    for r in bad:
        print(f"FAIL release gate: '{r}' is inside the DOTTY_RELEASE-only block")
    if not bad:
        print("PASS release gate: only dev keys are gated")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
