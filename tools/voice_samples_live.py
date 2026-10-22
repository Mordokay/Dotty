#!/usr/bin/env python3
"""Records voice samples through Gemini Live (the API Dotty talks with) for voices that
tools/voice_samples.py couldn't do: the free text-to-speech tier stops after about 10
requests a day, Live has its own allowance and is exactly the voice Dotty speaks with.

    GEMINI_API_KEY=... .venv/bin/python tools/voice_samples_live.py [VOICE ...]

Asks the model to say voice_samples.LINES word for word, collects the 24 kHz PCM reply and
saves ios/Dotty/Resources/Voices/<Voice>.m4a (afconvert). Skips voices that have a file.
"""
import asyncio
import base64
import json
import os
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import websockets

sys.path.insert(0, str(Path(__file__).resolve().parent))
from voice_samples import LINES, OUT, VOICES  # noqa: E402

MODEL = "models/gemini-3.8-live"
URL = "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"


async def record(key, voice):
    async with websockets.connect(f"{URL}?key={key}", max_size=None) as ws:
        await ws.send(json.dumps({"setup": {
            "model": MODEL,
            "generationConfig": {
                "responseModalities": ["AUDIO"],
                "speechConfig": {"voiceConfig": {"prebuiltVoiceConfig": {"voiceName": voice}}},
            },
            "systemInstruction": {"parts": [{"text": "You read lines aloud exactly as written, warmly and naturally, "
                                                     "with nothing added before or after."}]},
        }}))
        pcm = bytearray()
        async for raw in ws:
            message = json.loads(raw)
            if "setupComplete" in message:
                await ws.send(json.dumps({"clientContent": {
                    "turns": [{"role": "user", "parts": [{"text": f"Say exactly this: {LINES}"}]}],
                    "turnComplete": True}}))
                continue
            content = message.get("serverContent", {})
            for part in content.get("modelTurn", {}).get("parts", []):
                if "inlineData" in part:
                    pcm += base64.b64decode(part["inlineData"]["data"])
            if content.get("turnComplete"):
                return bytes(pcm)
    raise RuntimeError("connection closed before the reply ended")


def main():
    key = os.environ.get("GEMINI_API_KEY") or sys.exit("set GEMINI_API_KEY")
    for voice in sys.argv[1:] or list(VOICES):
        target = OUT / f"{voice}.m4a"
        if target.exists():
            continue
        pcm = asyncio.run(record(key, voice))
        with tempfile.TemporaryDirectory() as tmp:
            wav = Path(tmp) / "sample.wav"
            with wave.open(str(wav), "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(24000)
                w.writeframes(pcm)
            subprocess.run(["afconvert", "-f", "m4af", "-d", "aac", "-b", "64000", str(wav), str(target)], check=True)
        print(f"{voice}: {len(pcm) / 48000:.1f} s, {target.stat().st_size // 1024} KB", flush=True)


if __name__ == "__main__":
    main()
