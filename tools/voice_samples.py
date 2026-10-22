#!/usr/bin/env python3
"""Records a sample of every Gemini voice for the iOS app's voice picker (the AI companion).

    GEMINI_API_KEY=... python3 tools/voice_samples.py [VOICE ...]

Each voice says the same lines through Gemini's text-to-speech (the same voices Gemini Live
speaks with on Dotty); the WAV is converted to AAC (.m4a, afconvert) into
ios/Dotty/Resources/Voices/<Voice>.m4a, bundled with the app so it plays offline and the app
needs no key. Voices that already have a file are skipped. Retries on rate limits (the free
tier allows only a few speech requests a minute).
"""
import base64
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "ios" / "Dotty" / "Resources" / "Voices"
MODEL = "gemini-3.8-flash-tts"
LINES = "Hi, I'm Dotty. I can be your coach, your friend, or just someone to talk to. So, how was your day?"

# Gemini's prebuilt voices and Google's one-word style for each (keep in sync with the app).
VOICES = {
    "Zephyr": "Bright", "Puck": "Upbeat", "Charon": "Informative", "Kore": "Firm",
    "Fenrir": "Excitable", "Leda": "Youthful", "Orus": "Firm", "Aoede": "Breezy",
    "Callirrhoe": "Easy-going", "Autonoe": "Bright", "Enceladus": "Breathy", "Iapetus": "Clear",
    "Umbriel": "Easy-going", "Algieba": "Smooth", "Despina": "Smooth", "Erinome": "Clear",
    "Algenib": "Gravelly", "Rasalgethi": "Informative", "Laomedeia": "Upbeat", "Achernar": "Soft",
    "Alnilam": "Firm", "Schedar": "Even", "Gacrux": "Mature", "Pulcherrima": "Forward",
    "Achird": "Friendly", "Zubenelgenubi": "Casual", "Vindemiatrix": "Gentle", "Sadachbia": "Lively",
    "Sadaltager": "Knowledgeable", "Sulafat": "Warm",
}


def speak(key, voice):
    body = json.dumps({
        "contents": [{"parts": [{"text": LINES}]}],
        "generationConfig": {
            "responseModalities": ["AUDIO"],
            "speechConfig": {"voiceConfig": {"prebuiltVoiceConfig": {"voiceName": voice}}},
        },
    }).encode()
    url = f"https://generativelanguage.googleapis.com/v1beta/models/{MODEL}:generateContent"
    for attempt in range(8):
        request = urllib.request.Request(url, body, {"x-goog-api-key": key, "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                data = json.load(response)
            part = data["candidates"][0]["content"]["parts"][0]["inlineData"]
            return base64.b64decode(part["data"]), part["mimeType"]
        except urllib.error.HTTPError as e:
            if e.code in (429, 500, 503):
                wait = 25 * (attempt + 1)
                print(f"  {voice}: HTTP {e.code}, waiting {wait} s", flush=True)
                time.sleep(wait)
                continue
            raise SystemExit(f"{voice}: HTTP {e.code} {e.read()[:300]!r}")
    raise SystemExit(f"{voice}: gave up after retries")


def main():
    key = os.environ.get("GEMINI_API_KEY")
    if not key:
        raise SystemExit("set GEMINI_API_KEY")
    OUT.mkdir(parents=True, exist_ok=True)
    voices = sys.argv[1:] or list(VOICES)
    for voice in voices:
        target = OUT / f"{voice}.m4a"
        if target.exists():
            continue
        audio, mime = speak(key, voice)
        with tempfile.TemporaryDirectory() as tmp:
            wav = Path(tmp) / "sample.wav"
            if mime.startswith("audio/wav"):
                wav.write_bytes(audio)
            else:  # raw 16-bit PCM (audio/L16;rate=24000): wrap it in a WAV header
                import wave
                with wave.open(str(wav), "wb") as w:
                    w.setnchannels(1), w.setsampwidth(2), w.setframerate(24000)
                    w.writeframes(audio)
            subprocess.run(["afconvert", "-f", "m4af", "-d", "aac", "-b", "64000", str(wav), str(target)], check=True)
        print(f"{voice}: {target.stat().st_size // 1024} KB", flush=True)
        time.sleep(4)


if __name__ == "__main__":
    main()
