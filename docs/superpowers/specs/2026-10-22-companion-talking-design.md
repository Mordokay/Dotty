# Companion, piece 1: Talking — design

Date: 2026-10-22. Status: approved in conversation, section by section; this is the written spec.

## Goal and scope

A new `companion` cartridge that turns Dotty into a talking AI companion: hold BOOT and speak,
Dotty answers out loud in about a second, shows the reply as text, and has an animated avatar
with expressions. English only.

In this piece: talking (push-to-talk, streamed replies), text-only mode, 10 avatars with
expressions and animation, the `set_mood` action, history on the card, usage counting, the app's
Companion screen (status, avatar, voice, text-only, API key, history).

Not in this piece (later pieces, but this design leaves room for them): roles and memory
(piece 2), scheduled moments and the heartbeat (piece 3), the iPhone link and the Bluetooth relay
through the iPhone's mobile data (piece 4).

## Background (from the research and the test run, 2026-10-22)

- Engine: **Gemini Live API**, model `gemini-3.8-live`, over one TLS WebSocket:
  `wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=KEY`.
  Input 16 kHz 16-bit mono PCM, output 24 kHz 16-bit mono PCM, base64 in JSON messages.
  Push-to-talk: automatic activity detection off, `activityStart` / audio chunks / `activityEnd`.
- The throwaway test on Dotty (Arduino core, BLE stopped): Wi-Fi ~5 s, TLS + setup ~8 s, first
  reply audio 0.8-1.3 s after releasing BOOT, 5 turns stable, internal heap 116 KB free (largest
  block 73 KB) and flat, speech recognition exact.
- Free tier: no card, works from Portugal. The daily allowance resets at midnight Pacific time
  (08:00 Lisbon in winter, 09:00 in summer). Google exposes no usage or remaining-quota API for an
  API key; the Live API reports tokens per turn (`usageMetadata`).
- Gemini's 30 prebuilt voices; samples are in the app (`ios/Dotty/Resources/Voices`).
- Free Gemini text-to-speech stops at ~10 requests a day: not used here.

## Components

### Firmware: `cartridges/companion/`

| Unit | Does | Depends on |
|---|---|---|
| `gemini_live.*` | Our own small WebSocket client over `esp_tls` with Dotty's certificate bundle (`esp_crt_bundle_attach`), frames assembled in PSRAM (no message size limit); the Live protocol: setup, audio chunks, activity start/end, receiving audio / transcripts / tool calls / usage / errors / goAway, session resumption. Exposes events to the caller; knows nothing about the screen or audio hardware. | `net` (Wi-Fi), PSRAM |
| `voice.*` | Push-to-talk capture (reader task → PSRAM stream buffer → 100 ms chunks to `gemini_live`, like Tape's recorder), the microphone level for the meter, and playback of the reply as it streams in. | `AudioPlayer` (+ the new streaming API) |
| `avatar.*` | The 10 avatars: base pictures + the face kit (expressions), the animation clock (a frame every 1-2 s), drawing at 96×96 and the small lock-screen size. | `images/avatars.h` |
| `history.*` | Appends each finished turn to `data/history/YYYY-MM-DD.jsonl`; lists days; reads a day; deletes. | `storage` |
| `usage.*` | Counts turns and tokens per "Google day" (reset 08:00/09:00 local, from the phone's UTC offset); remembers "allowance used up until <time>". NVS `companion`. | `shell::utcOffset` |
| `main.cpp` | States, screens, controls, settings (NVS `companion`: key, voice, avatar, textOnly), BLE commands. | all of the above, `shell` |

The Gemini part and the audio transport stay separate so piece 4's Bluetooth relay can replace
the Wi-Fi WebSocket without touching the screen or audio code.

### Shared code (dotty_core): one addition

`AudioPlayer` gains streaming PCM playback: `bool startStream(uint32_t sampleRate)`,
`size_t writeStream(const int16_t *mono, size_t frames)` (into a PSRAM ring buffer the audio task
drains), `void endStream()` (play out what's left), `void stopStream()` (cut it off), and
`bool streaming()`. Switching TX to 24 kHz and back to the capture rate happens there. Shared, so
every cartridge is bumped when it ships.

### App: Companion screen (`ios/Dotty/Features/Companion/`)

Shown on the dashboard when the companion runs (`DashboardView.route`). Sections: status
(connected, today's chats and tokens, "out of words until 08:00"), avatar grid (10, colour
previews), voice list (all 30 with play buttons, Zephyr and Achernar pinned first; default
Zephyr), text-only toggle, API key (entry field; afterwards only the last 4 characters shown; a
link to Google AI Studio), history (days → chat bubbles). The temporary Settings › Companion
voices row is removed (its view moves here).

## Conversation flow

- **Connecting:** opening the cartridge or unlocking starts a background connect (Wi-Fi, TLS,
  setup). The avatar looks sleepy ("Waking up…"). BLE is stopped while connected (TLS needs the
  internal RAM), and the shell keeps Dotty awake. Disconnects after 3 minutes without a turn, or
  on lock; the next BOOT press reconnects (one ~8 s wait).
- **A turn:** hold BOOT (≥ 120 ms, as in Tape) → listening face, level meter, audio streams up
  in 100 ms chunks, your words shown as the input transcript arrives; release → `activityEnd`,
  thinking face; the reply plays as it streams in (first audio ~1 s), the mouth animates, the
  reply text appears; at the end the mood face shows for a few seconds, then idle. The turn is
  saved to history and counted in usage.
- **Interrupting:** BOOT while it's talking stops playback and starts listening.
- **Text-only mode:** `responseModalities: ["TEXT"]`, no audio; the text fills the screen with a
  small avatar.
- **Long replies:** the text scrolls with swipe up/down after the reply; audio plays in full.
- **Sessions:** on `goAway` (≈10 min per connection) reconnect with the session-resumption
  handle, so the conversation continues.

## The AI side

Setup message per connection: model, voice (the app's pick; Dotty has no voice switcher),
transcripts on (input and output), automatic activity detection off, and a system instruction:

> You are Dotty, a small desk companion. Talk like a person in conversation: usually one or two
> short sentences; longer only when asked. You are on a tiny screen with a speaker: never use
> lists, markdown or emoji. Call set_mood with each reply. The local time is {time}, the user is
> in {place}.

(`{place}` from the Weather cartridge's saved location if any; omitted otherwise.)

**Dotty actions, first one:** a function declaration `set_mood(mood)` with mood one of
`happy, sad, surprised, thinking, laughing, love, neutral`. Dotty answers each tool call
immediately (`toolResponse` "ok") and shows the face; no call → neutral. Later pieces add
`remember`, `recall`, `schedule_moment`, `set_alarm` the same way.

**History line** (`data/history/YYYY-MM-DD.jsonl`):
`{"t": <local epoch>, "you": "...", "dotty": "...", "mood": "happy", "ms": <reply time>, "tokens": n}`

## Screen (200×200 e-paper)

- **Idle:** avatar 96×96 in the top two thirds, animating every 1-2 s (blink, bob, look around);
  one status line ("Hold BOOT to talk", "Waking up…", "No Wi-Fi", "Key missing"); top-left
  corner = text-only toggle (speech bubble, filled when on), top-right = history (last few
  exchanges, scrollable).
- **Listening:** listening face, mic level meter (~2 updates a second), your words.
- **Thinking:** thinking face. **Talking:** two mouth shapes alternating, reply text (2-3 lines,
  newest words). **After:** mood face a few seconds.
- **Text-only:** small avatar in a corner, text gets the screen.
- **Lock screen** (`Config::lockScreen`): a smaller avatar (sleeping or idle) beside the clock,
  updated each minute; a mark while still connected.
- Refreshes: partial while animating, a full refresh every ~2 minutes (the Pet's rule).

## Avatars

Ten: Robot, Dog, Cat, Fox, Owl, Bear, Girl (blonde), Guy, Ghost, Dragon. Style: modern Disney
(e.g. Tangled): cute faces, big glossy expressive eyes, soft rounded shapes. Made like the Pet:
a prompt pack (`docs/companion-avatar-prompts.md`) → the user generates the pictures (black lines
on white, face left empty) → `tools/avatar_art.py` turns them into 96×96 1-bit art + a face kit
drawn by code in that style (big eyes with highlights, eyebrows that carry emotion, small
mouths), tuned per avatar. Expressions: idle, blink, listening, thinking, talking A/B, happy,
sad, surprised, laughing, love, sleeping. The blonde girl's hair is a light outline with a bit of
texture (not solid black). Colour previews of the same pictures for the app.

## Settings and BLE commands

NVS `companion`: `key` (the Gemini API key), `voice` (default "Zephyr"), `avatar`, `textOnly`,
usage counters. Commands (app ↔ Dotty, only while BLE is on, i.e. not mid-conversation):

- `companion.status` → {connected, state, mood, avatar, voice, textOnly, keySet, keyHint
  (last 4), today{turns, tokens}, outUntil?}
- `companion.settings {avatar?, voice?, textOnly?, key?}` (the key is set over the encrypted,
  bonded BLE link; never logged)
- `companion.history {day?}` → {days[]} or {turns[]}
- `companion.forget {day?}`
- Event `companion.changed`

## Errors

| Case | Dotty | App |
|---|---|---|
| No key | "Key missing — add it in the app" | key field highlighted |
| Key rejected | "Key not accepted" | hint to check it |
| Free allowance used up (HTTP 429 / quota error) | "I'm out of words for today. Back at 08:00." (the error's retry delay when it has one) | same line in status |
| No Wi-Fi | "No Wi-Fi", retries on the next press | — |
| Drop mid-reply | keeps what arrived, saves it, reconnects quietly | — |
| goAway | reconnects with resumption | — |

## Testing

- Bench, with serial dev keys: simulated BOOT hold, text-only toggle, each mood in turn, a fake
  quota error, a forced goAway.
- Measured on the device: time to first audio (target ≈1 s with streaming), internal heap
  through long chats, a session past 10 minutes (resumption), battery over an hour of chatting.
- The user: real conversations in both modes; avatar and voice choice from the app; history.

## Housekeeping

Delete `cartridges/aispike/` (and its key) and the `[env:aispike]` entry; new `[env:companion]`;
`cartridge.json`, `icon.png`, `artwork.svg/png`; bump every cartridge for the AudioPlayer
change; CLAUDE.md section; catalog release.
