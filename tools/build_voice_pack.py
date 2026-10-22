#!/usr/bin/env python3
"""Packs the app's voice samples (ios/Dotty/Resources/Voices/*.m4a) into
cartridges/companion/voices.bin, which the companion firmware embeds so Dotty can play a voice
as you step through them in its settings.

Each sample becomes a 48 kbps mono 32 kHz MP3, no ID3 or Xing header (Dotty decodes MP3, not AAC;
24 kHz made MPEG-2 frames the player rejected ("No MP3 frames"); ~45 KB each, 1.4 MB for
all 30). File: "DVS1", a count, then {name[16], offset, size} per sample (little-endian), then
the MP3s back to back (read by cartridges/companion/voice_pack.h).

    python3 tools/build_voice_pack.py      # needs ffmpeg (brew install ffmpeg)
"""
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "ios/Dotty/Resources/Voices"
OUT = ROOT / "cartridges/companion/voices.bin"


def mp3(m4a: Path, tmp: Path) -> bytes:
    out = tmp / (m4a.stem + ".mp3")
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(m4a), "-ac", "1", "-ar", "32000",
                    "-b:a", "48k", "-map_metadata", "-1", "-id3v2_version", "0", "-write_xing", "0", str(out)], check=True)
    return out.read_bytes()


def main() -> None:
    samples = sorted(SRC.glob("*.m4a"))
    with tempfile.TemporaryDirectory() as tmp:
        blobs = [(m.stem, mp3(m, Path(tmp))) for m in samples]
    header = b"DVS1" + struct.pack("<I", len(blobs))
    offset = len(header) + 24 * len(blobs)
    index, data = b"", b""
    for name, blob in blobs:
        assert len(name) < 16, name
        index += name.encode().ljust(16, b"\0") + struct.pack("<II", offset + len(data), len(blob))
        data += blob
    OUT.write_bytes(header + index + data)
    print(f"{len(blobs)} voices, {OUT.stat().st_size // 1024} KB -> {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
