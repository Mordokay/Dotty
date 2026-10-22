# Companion, piece 1 (Talking) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `companion` cartridge: hold BOOT and talk to Gemini Live, hear a streamed reply in ~1 s with the text on screen, an animated avatar with moods, history and usage on the card, and an app Companion screen.

**Architecture:** Pure, host-testable C++ for the WebSocket frame codec, the Live protocol messages and the usage/history rules (std::string + ArduinoJson, built with clang on the Mac like `tools/pet_sim`); thin device glue for TLS (`esp_tls` + certificate bundle), audio (a new streaming PCM API in `AudioPlayer`), the screen and BLE. The Gemini session object knows nothing about the screen or audio hardware, so piece 4's Bluetooth relay can replace its transport.

**Tech Stack:** Arduino-ESP32 3.x (pioarduino), ESP-IDF `esp_tls`, ArduinoJson 7, mbedTLS base64, Adafruit GFX, NimBLE; SwiftUI app; Python 3 + Pillow for art; clang++ for host tests.

**Spec:** `docs/superpowers/specs/2026-10-22-companion-talking-design.md`

## Global Constraints

- English only. Model `models/gemini-3.8-live`. Endpoint `wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=KEY`.
- Audio: 16 kHz 16-bit mono PCM up (`audio/pcm;rate=16000`, 100 ms = 1600-frame chunks); 24 kHz 16-bit mono PCM down.
- Default voice `Zephyr`; Dotty uses the app's pick only (no voice switcher on Dotty).
- System instruction (exact): "You are Dotty, a small desk companion. Talk like a person in conversation: usually one or two short sentences; longer only when asked. You are on a tiny screen with a speaker: never use lists, markdown or emoji. Call set_mood with each reply. The local time is {time}, the user is in {place}." (`, the user is in {place}` omitted when no place is known).
- Moods: `happy, sad, surprised, thinking, laughing, love, neutral`.
- History: `data/history/YYYY-MM-DD.jsonl`, one line `{"t":<local epoch>,"you":"…","dotty":"…","mood":"…","ms":<n>,"tokens":<n>}`.
- NVS namespace `companion`: `key`, `voice`, `avatar`, `textOnly`, usage counters. The key is never logged (log only its last 4 characters).
- Disconnect after 3 minutes without a turn, or on lock. BLE is stopped while connected.
- Free allowance resets at midnight US Pacific time (`America/Los_Angeles`, DST-aware).
- Push-to-talk: BOOT held ≥ 120 ms (as in Tape).
- Firmware rules from CLAUDE.md: LOGI/LOGW/LOGE only (never `Serial.print`); screen text through `ui::printable`; commits as Pedro Saldanha <papajorgioster@gmail.com> with the Co-Authored-By line; a cartridge change ends with commit → push → `.venv/bin/python tools/build_catalog.py --release`; a `lib/dotty_core` change bumps every cartridge.

## Review Focus

- Wi-Fi or the WebSocket drops while BOOT is held or the reply is playing → status shows the problem, capture/playback stop, no hang; the next press reconnects. (Test: Task 5 dev key `X` forces a socket close mid-turn.)
- Holding BOOT for a very long time (> 60 s) or a very long reply → memory stays bounded (capture streams and drops nothing; the playback ring blocks the reader, never grows). (Test: Task 1 ring overflow test; Task 5 `r` held 70 s.)
- Locking while connected or talking → playback/capture stop, socket closes, BLE comes back on unlock. (Test: Task 5 step with `k` mid-reply.)
- A key pasted with spaces or a newline, or an empty key, from the app → trimmed; empty = "Key missing". (Test: Task 6 `settingsKey` host test.)
- The Pacific reset across daylight-saving changes (US and EU switch on different weekends) → correct local reset time. (Test: Task 4 host tests for 2026-03-10 and 2026-10-28.)

---

### Task 1: Streaming PCM playback in AudioPlayer (shared)

**Files:**
- Modify: `lib/dotty_core/src/audio_player.h`, `lib/dotty_core/src/audio_player.cpp`
- Create: `lib/dotty_core/src/pcm_ring.h` (header-only ring buffer, host-testable)
- Test: `tools/companion_test/test_pcm_ring.cpp`, runner `tools/companion_test/run.py`

**Interfaces:**
- Produces: `class PcmRing { PcmRing(int16_t *storage, size_t frames); size_t write(const int16_t *src, size_t n); size_t read(int16_t *dst, size_t n); size_t available() const; size_t space() const; void clear(); }` (single producer / single consumer, no allocation).
- Produces on `AudioPlayer`: `bool startStream(uint32_t sampleRate);` (stops other playback/capture, switches TX + codec to the rate, allocates a 2-second PSRAM ring once), `size_t writeStream(const int16_t *mono, size_t frames);` (copies what fits, returns frames taken; never blocks), `void endStream();` (plays out the rest, then `streaming()` turns false), `void stopStream();` (cuts off now), `bool streaming() const;`. The audio task drains the ring in 256-frame blocks to stereo, writes silence on underrun (counts underruns, `LOGI` once per stream with the count).

- [ ] **Step 1: Write the failing ring tests** in `tools/companion_test/test_pcm_ring.cpp`: `write` of 300 frames into a 256-frame ring returns 256; `read` 100 then `write` 100 wraps and `available()==256`; reading from empty returns 0; `clear()` empties.
- [ ] **Step 2: Write `tools/companion_test/run.py`** (like `tools/pet_sim/compare.py`): compiles each `test_*.cpp` with `clang++ -std=c++17 -I lib/dotty_core/src -I cartridges/companion -I <ArduinoJson include>` (ArduinoJson from the PlatformIO libdeps; fail with a clear message if absent: "build any env once first") and runs it; a test prints `PASS name` / `FAIL name: detail` and exits non-zero on failure. Run: `python3 tools/companion_test/run.py` → FAIL (no `pcm_ring.h`).
- [ ] **Step 3: Implement `pcm_ring.h`.** Run tests → PASS.
- [ ] **Step 4: Implement the stream API in `AudioPlayer`** (state `streaming_`, ring in PSRAM via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`; the existing `run()` loop gets a branch that drains the ring like `playWav` does; holds `power::kWakeLockAudio` while streaming).
- [ ] **Step 5: Device check.** Create the minimal `cartridges/companion/main.cpp` (`DOTTY_CARTRIDGE("companion", "Companion", "0.1.0")`, shell + "Companion" placeholder screen) and `[env:companion]` in `platformio.ini`; dev key `S` streams a 2-second 440 Hz sine at 24 kHz through `writeStream` in 480-frame pieces. Run: `pio run -e companion -t upload`, send `S`, read the log (`d`). Expected: a clean 2 s tone, log `stream: 0 underruns`. Also `pio run -e music` builds.
- [ ] **Step 6: Commit** (`pcm_ring.h`, audio_player, companion skeleton, platformio.ini env, tools/companion_test).

### Task 2: WebSocket client (frames + TLS)

**Files:**
- Create: `cartridges/companion/ws_frame.h/.cpp` (pure), `cartridges/companion/ws_client.h/.cpp` (device)
- Test: `tools/companion_test/test_ws_frame.cpp`

**Interfaces:**
- Produces (pure): `std::string wsHandshake(const std::string &host, const std::string &path, const std::string &key16b64);` `bool wsCheckAccept(const std::string &responseHeaders, const std::string &key16b64);` `std::vector<uint8_t> wsEncode(uint8_t opcode, const uint8_t *data, size_t len, const uint8_t mask[4]);` (client frames are masked; 7/16/64-bit lengths) and `struct WsDecoder { enum Result { NeedMore, Message, Ping, Close, Error }; Result feed(const uint8_t *data, size_t len, size_t &used); std::string message; uint8_t opcode; uint16_t closeCode; }` (reassembles fragmented messages into `message`, no size limit; text and binary both delivered as `Message`).
- Produces (device): `class WsClient { bool connect(const char *host, const char *path, String &error); bool sendText(const char *data, size_t len); void poll(std::function<void(const std::string &)> onMessage); void close(); bool connected() const; }` — `esp_tls_conn_new_sync` with `crt_bundle_attach = esp_crt_bundle_attach`, port 443, 15 s timeout; message storage in PSRAM (custom allocator for `std::string` is not needed: reserve via `heap_caps_malloc`-backed buffer inside `WsDecoder` on device builds, `std::string` on host); answers pings with pongs; `poll` is non-blocking (`esp_tls_get_bytes_avail` / non-blocking read).

- [ ] **Step 1: Write failing tests:** RFC 6455 accept for key `dGhlIHNhbXBsZSBub25jZQ==` is `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`; `wsEncode(0x1, "Hi", mask {1,2,3,4})` = `81 82 01 02 03 04 49 6b`; a 70,000-byte payload uses the 64-bit length form; the decoder reassembles a text message split into two fragments fed one byte at a time; a ping returns `Ping`; a close frame returns `Close` with code 1000; a 300 KB message decodes intact.
- [ ] **Step 2: Run** `python3 tools/companion_test/run.py` → FAIL.
- [ ] **Step 3: Implement `ws_frame.*`** (SHA-1 for the accept: mbedTLS on device, a small portable SHA-1 in the file under `#ifndef ARDUINO` for the host). Run → PASS.
- [ ] **Step 4: Implement `ws_client.*`.** Device check: dev key `W` connects to `echo.websocket.org`, sends "hello", logs the echoed text and heap (internal free / largest block) before and after TLS. Expected log: `ws: echo "hello"`, TLS connect < 10 s with BLE stopped.
- [ ] **Step 5: Commit.**

### Task 3: Gemini Live protocol and session

**Files:**
- Create: `cartridges/companion/live_protocol.h/.cpp` (pure), `cartridges/companion/gemini_live.h/.cpp` (device)
- Test: `tools/companion_test/test_live_protocol.cpp`

**Interfaces:**
- Consumes: `WsClient` (Task 2).
- Produces (pure): `struct LiveConfig { std::string model, voice, systemInstruction, resumeHandle; bool textOnly; };` `std::string liveSetup(const LiveConfig &);` (generationConfig.responseModalities AUDIO or TEXT, speechConfig voice, systemInstruction, `realtimeInputConfig.automaticActivityDetection.disabled=true`, `inputAudioTranscription`, `outputAudioTranscription`, tools with the `set_mood` function declaration (mood: string enum of the 7 moods), `sessionResumption {handle}`); `std::string liveActivityStart();` `std::string liveActivityEnd();` `std::string liveAudio(const int16_t *pcm, size_t frames);` `std::string liveToolOk(const std::string &callId, const std::string &name);` `struct LiveEvent { bool setupComplete, turnComplete, interrupted, goAway; std::string audioPcm /*decoded*/, text, inputText, outputText, toolCallId, toolName, mood, resumeHandle, error; int errorCode = 0; int retryDelaySeconds = -1; int totalTokens = -1; };` `LiveEvent liveParse(const std::string &message);` (filtered ArduinoJson parse; `error.code`/HTTP-style codes; quota detected by code 429 or status `RESOURCE_EXHAUSTED`; `retryDelay` like `"37s"` → 37); `std::string systemInstruction(const std::string &localTime, const std::string &place);` (the Global Constraints text).
- Produces (device): `class GeminiLive { struct Callbacks { std::function<void()> ready; std::function<void(const int16_t*, size_t)> audio; std::function<void(const std::string&)> heard, said; std::function<void(const std::string&)> mood; std::function<void(int tokens)> turnDone; std::function<void(const std::string &error, int retryDelaySeconds, bool quota)> failed; }; bool begin(const LiveConfig &, Callbacks); void startTurn(); void sendAudio(const int16_t *, size_t); void endTurn(); void poll(); void close(); bool ready() const; }` — answers `set_mood` tool calls with `liveToolOk` immediately; on `goAway` reconnects with the last resume handle.

- [ ] **Step 1: Write failing tests:** `liveSetup` JSON has `setup.model=="models/gemini-3.8-live"`, voice `Zephyr`, `automaticActivityDetection.disabled==true`, a `set_mood` declaration whose enum is exactly the 7 moods, and `responseModalities==["TEXT"]` when `textOnly`; `liveAudio` of 2 frames {1,-1} yields `"data":"AQD//w=="` and `"mimeType":"audio/pcm;rate=16000"`; `liveParse` on recorded sample messages (put verbatim samples from the test run in `tools/companion_test/live_samples/`): setupComplete, a modelTurn with inlineData (decoded byte count right), outputTranscription text, toolCall `set_mood {"mood":"happy"}` → `mood=="happy"` and the call id, `usageMetadata.totalTokenCount` → `totalTokens`, goAway, `sessionResumptionUpdate.newHandle`, and an error `{"error":{"code":429,"status":"RESOURCE_EXHAUSTED","details":[{"retryDelay":"37s"}]}}` → `errorCode 429`, `retryDelaySeconds 37`; `systemInstruction("14:05", "")` omits the place clause.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement `live_protocol.*`.** Run → PASS.
- [ ] **Step 4: Implement `gemini_live.*`.** Device check: dev key `c` with the key in NVS (dev key `K` reads a key typed after it over serial, for the bench) connects; log `live: ready in N ms`; dev key `T` sends a text turn "Say hello" (clientContent) and logs the transcript and token count.
- [ ] **Step 5: Commit.**

### Task 4: Usage and history

**Files:**
- Create: `cartridges/companion/usage_rules.h/.cpp` (pure), `cartridges/companion/history.h/.cpp` (device, SD)
- Test: `tools/companion_test/test_usage_rules.cpp`

**Interfaces:**
- Produces (pure): `int64_t nextPacificMidnightUtc(int64_t nowUtc);` (US DST rule: second Sunday of March 02:00 → first Sunday of November 02:00), `std::string historyLine(int64_t localEpoch, const std::string &you, const std::string &dotty, const std::string &mood, int ms, int tokens);` (JSON-escaped), `std::string dayFile(int64_t localEpoch);` → `"2026-10-22.jsonl"`, `std::string trimKey(const std::string &raw);`.
- Produces (device): `namespace history { void append(...same fields...); std::vector<String> days(); String day(const String &yyyymmdd); bool forget(const String &day /* "" = all */); }` and `namespace usage { void addTurn(int tokens); int turnsToday(); int tokensToday(); void markOut(int64_t untilUtc); int64_t outUntilUtc(); /* 0 if not out */ }` (NVS `companion`; counters reset when `now ≥ nextPacificMidnightUtc(the last reset)`).

- [ ] **Step 1: Write failing tests:** for `nowUtc` = 2026-10-22 12:00 UTC the next Pacific midnight is 2026-10-23 07:00 UTC (PDT); for 2026-11-05 12:00 UTC it is 2026-11-06 08:00 UTC (PST); for 2026-03-10 12:00 UTC it is 2026-03-11 07:00 UTC (PDT started 03-08); `historyLine` escapes a quote and a newline and matches `{"t":…,"you":"…","dotty":"…","mood":"happy","ms":950,"tokens":412}`; `dayFile` for 2026-10-22 23:59 local = `2026-10-22.jsonl`; `trimKey("  AIza123 \n")=="AIza123"`, `trimKey("\n")==""`.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** Run → PASS.
- [ ] **Step 4: Implement `history.*` and `usage.*`** (device). Device check: dev key `H` appends a fake turn and logs `history: <day> 1 turn`.
- [ ] **Step 5: Commit.**

### Task 5: Talking on Dotty (voice + conversation + screen, placeholder avatar)

**Files:**
- Create: `cartridges/companion/voice.h/.cpp`, `cartridges/companion/screens.h/.cpp`
- Modify: `cartridges/companion/main.cpp`

**Interfaces:**
- Consumes: `AudioPlayer` stream API (Task 1), `GeminiLive` (Task 3), `history`/`usage` (Task 4).
- Produces: `namespace voice { void begin(AudioPlayer &); bool startListening(); /* capture task → 1600-frame chunks */ void stopListening(); bool nextChunk(int16_t *out, size_t &frames); int levelDb(); void playStart(); void play(const int16_t *pcm, size_t frames); void playEnd(); void playStop(); bool playing(); }`; states `enum class Talk { Offline, Connecting, Ready, Listening, Thinking, Speaking, After, Error }`; `screens::draw(Talk, const ScreenInfo &)` with `ScreenInfo { String status, heard, said; int levelDb; String mood; bool textOnly; }` (avatar drawn by a stub until Task 6: a circle with the mood name).

- [ ] **Step 1: Implement voice + the state machine + screens** per the spec's flow: background connect on open/unlock, BLE stopped while connected, `shell::wake()` while connected and not idle-timed-out, 3-minute idle disconnect, BOOT ≥ 120 ms = listen, release = `endTurn`, stream reply audio into `voice::play` as it arrives, BOOT while speaking = `playStop` + listen, text-only mode (`textOnly` setting; top-left corner toggles), top-right corner = the last few exchanges from history (scrollable), swipe up/down scrolls a long reply's text after it ends, `{place}` for the system instruction from NVS `weather` key `name` when set (omitted otherwise), `{time}` as "HH:MM" local, lock = close + stop everything, errors per the spec's table (quota → `usage::markOut(retry or nextPacificMidnightUtc)` and "I'm out of words for today. Back at HH:MM." in local time). Dev keys: `r` simulated BOOT hold, `t` text-only toggle, `X` force socket close, `m` cycle moods on the stub avatar, `q` fake quota error.
- [ ] **Step 2: Device test — latency and flow.** Flash, set the key with `K`, hold BOOT (or `r`) and ask a question. Expected log: `live: first audio N ms after release` with N ≈ 1000, audio heard while still arriving (no wait for the end), history line written, usage +1.
- [ ] **Step 3: Device test — Review Focus cases.** `X` while BOOT held → "Connection lost", capture stops, next press reconnects; `r` held 70 s → heap stable (log internal/PSRAM free before and after); `k` (lock) mid-reply → playback stops, socket closed, unlock restarts BLE (log `ble: advertising`); `q` → the out-of-words line with the reset time; a session left open 11 minutes with a turn every few minutes → `live: resumed` and the conversation continues ("what did I just ask?").
- [ ] **Step 4: Commit.**

### Task 6: Settings over BLE

**Files:**
- Modify: `cartridges/companion/main.cpp` (or create `cartridges/companion/commands.cpp`)
- Test: covered by Task 4's `trimKey` test plus `tools/ble_dotty.py` checks

**Interfaces:**
- Produces the spec's commands: `companion.status`, `companion.settings {avatar?, voice?, textOnly?, key?}` (key trimmed with `trimKey`; empty clears it; only the last 4 characters ever logged or returned as `keyHint`), `companion.history {day?}`, `companion.forget {day?}`, event `companion.changed`.

- [ ] **Step 1: Implement the commands.** Settings changes apply at the next connect (a voice or text-only change while connected reconnects).
- [ ] **Step 2: Verify from the Mac:** `.venv/bin/python tools/ble_dotty.py cmd companion.settings voice=Achernar` then `cmd companion.status` → `voice: Achernar`, `keySet: true`, `keyHint` 4 characters; `cmd companion.history` lists today; `cmd companion.forget` empties it. Log contains no full key (`grep` the replayed log for the key's first 8 characters: no match).
- [ ] **Step 3: Commit.**

### Task 7: Avatars (art pipeline, face kit, animation, lock screen)

**Files:**
- Create: `docs/companion-avatar-prompts.md`, `tools/avatar_art.py`, `tools/avatar_art_src/` (the user's pictures, later), `cartridges/companion/images/avatars.h` (generated), `cartridges/companion/avatar.h/.cpp`, `tools/avatar_art_preview.png`, app previews `ios/Dotty/Resources/Avatars/<id>.png`
- Modify: `cartridges/companion/screens.cpp` (replace the stub), `main.cpp` (lock screen)

**Interfaces:**
- Produces: avatar ids `robot, dog, cat, fox, owl, bear, girl, guy, ghost, dragon`; `namespace avatar { void draw(Adafruit_GFX &, int16_t x, int16_t y, const char *id, Face face, int frame, int scale /*1 = 96 px, 0 = 48 px*/); enum class Face { Idle, Blink, Listening, Thinking, TalkA, TalkB, Happy, Sad, Surprised, Laughing, Love, Sleeping }; Face forMood(const std::string &mood); }`.

- [ ] **Step 1: Write the prompt pack** `docs/companion-avatar-prompts.md` (like `docs/pet-art-prompts.md`): one shared style block (modern Disney / Tangled: cute proportions, big round eyes area left EMPTY, soft rounded shapes, clean thick black outlines on pure white, no shading, no text, front-facing, head and shoulders, square canvas) + one prompt per avatar (the girl: blonde, long hair drawn as light outlines with a few strands, not filled black).
- [ ] **Step 2: Write `tools/avatar_art.py`** following `tools/pet_art.py`: a picture in `tools/avatar_art_src/<id>.png` is cropped, scaled to 96×96, thresholded, its face area at a per-avatar `FACE = (x, y, scale)`; without a picture, a simple drawn placeholder silhouette. The face kit is drawn by code in the Disney style (large eyes with two highlights, lids for blink and sleep, eyebrows per expression, small mouths; talk A/B; laughing = closed happy eyes + open mouth; love = heart eyes). Output: `images/avatars.h` (bitmaps per avatar: base + face layers), the preview sheet, and colour-free app previews (PNG 192×192, idle face). Run: `python3 tools/avatar_art.py` → writes the files; check `tools/avatar_art_preview.png` by eye.
- [ ] **Step 3: Implement `avatar.*` and the animation** (a frame every 1.5 s unlocked: blink every ~4 s, a 2 px bob, talk A/B while speaking; partial refreshes with `shell::refresh(false, false)` and a full refresh every 2 minutes, as in the Pet), the lock screen (48 px avatar, sleeping when disconnected, beside the clock, a small mark while still connected; `Config::lockScreen`), and wire moods (`set_mood`) to faces for 4 s after a reply.
- [ ] **Step 4: Device check:** screenshots (`tools/screenshot.py`) of idle, listening (with meter), talking, each mood (dev key `m`), text-only, lock screen; compare against the spec's screen section.
- [ ] **Step 5: Commit** (art and code; the user's pictures replace placeholders later with `python3 tools/avatar_art.py` and a rebuild).

### Task 8: App Companion screen

**Files:**
- Create: `ios/Dotty/Features/Companion/CompanionView.swift`, `ios/Dotty/Features/Companion/CompanionHistoryView.swift`
- Modify: `ios/Dotty/Features/Companion/VoicesView.swift` (becomes a picker: `selected: String`, `onPick: (String) -> Void`, Zephyr and Achernar pinned first), `ios/Dotty/Features/Dashboard/DashboardView.swift` (route `case "companion" where info.supports("companion"): return .companion`; remove the Settings › Companion voices row and `.voices` route)

**Interfaces:**
- Consumes: Task 6 commands and event; avatar previews from Task 7 (`Resources/Avatars/<id>.png`).

- [ ] **Step 1: Implement CompanionView** (status: connected / today's chats and tokens / "Out of words until 08:00"; avatar grid of 10; voice picker; text-only toggle; key field with "•••• 1a2b" once set and a link to `https://aistudio.google.com/apikey`; History row → CompanionHistoryView with days → chat bubbles; reload on `companion.changed`).
- [ ] **Step 2: Build:** `xcodebuild -project ios/Dotty.xcodeproj -scheme Dotty -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build` → BUILD SUCCEEDED. Install on the iPhone; set the key, avatar and voice; Dotty shows them (`companion.status`).
- [ ] **Step 3: Commit.**

### Task 9: Ship

**Files:**
- Delete: `cartridges/aispike/` (with `secret.h`), the `[env:aispike]` block in `platformio.ini`, the `cartridges/aispike/` line in `.git/info/exclude`
- Create: `cartridges/companion/cartridge.json` (description, `"requires": ["wifi"]`), `icon.png` (64×64 1-bit), `artwork.svg` + `artwork.png` (night sky / firefly style, a glowing speech bubble)
- Modify: every `cartridges/*/main.cpp` version (dotty_core changed), `CLAUDE.md` (a Companion section: protocol, WebSocket client, streaming API, settings, commands, measurements)

- [ ] **Step 1: Run all host tests** `python3 tools/companion_test/run.py` → all PASS; build every env.
- [ ] **Step 2: Bump versions, write docs, delete the test run; `git status` shows no `secret.h` and no aispike files.**
- [ ] **Step 3: Commit, push, `.venv/bin/python tools/build_catalog.py --release`;** confirm `companion` is in the published catalog.
