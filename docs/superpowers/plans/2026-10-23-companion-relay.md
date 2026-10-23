# Companion Relay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Companion chats go through the iPhone over Bluetooth when it's near (Wi-Fi raced as the fallback), with every companion feature unchanged.

**Architecture:** `GeminiLive` gets a transport interface: the existing WebSocket (Wi-Fi) or a relay transport that sends the same Live API JSON to the phone over BLE (voice as IMA ADPCM) and receives Gemini's messages without audio plus ADPCM reply frames. The app's app-wide `RelayService` holds the Gemini WebSocket and makes text-model calls for Dotty (`relay.fetch`). A host-tested path chooser races the phone against Wi-Fi and runs the grace window.

**Tech Stack:** ESP32-S3 Arduino (pioarduino, custom core: mbedTLS in PSRAM), NimBLE-Arduino 2.x, ArduinoJson 7, FreeRTOS message buffers; SwiftUI / CoreBluetooth / URLSessionWebSocketTask (Swift 6).

**Spec:** `docs/superpowers/specs/2026-10-23-companion-relay-design.md`. Reference code (throwaway, do not merge): branch `spike/ble-relay` — `cartridges/companion/adpcm.h`, `ios/Dotty/Features/Relay/{Adpcm,RelayService}.swift`, the spike commands in `cartridges/companion/main.cpp`, `DottyLink` changes.

## Global Constraints

- Bluetooth only while Dotty is unlocked; off while locked (no `bluetoothWhileLocked`); a grace window in a Wi-Fi chat, nothing more.
- Audio: IMA ADPCM, 4 bits a sample, frames of 570 samples carrying their own start state (predictor int16 LE + index); voice 16 kHz up, reply 24 kHz down.
- Dotty → phone: JSON events on Event (pieces: every piece but the last starts with 0x1E); voice as raw notifications `0x1F 0x01 seq` + frame. Phone → Dotty: Data writes without response — `0x01 seq` + frame (audio), `0x02 flags` + bytes (JSON piece; flags bit 0 = more follows).
- Events: `relay.open {setup, key, resume?}`, `relay.send {message}`, `relay.fetch {id, model, body}`, `relay.close`. Phone JSON: Gemini's messages without `inlineData`, `{relay: "fetched", id, status, body}`, `{relay: "closed", code, reason}`. Handshake command `relay.hello {version, keys}` (keys = last 4 characters).
- The key never leaves the Keychain or Dotty: only its last 4 characters travel. The phone only calls `generativelanguage.googleapis.com` (Live WebSocket and `generateContent`).
- Grace window: Bluetooth stays on until the end of the first reply or 15 s after the Wi-Fi session is ready, whichever is later; a phone that connects in it takes the chat at the next press, resuming the conversation (session-resume handle).
- Fetch timeout 30 s; relay protocol version 1; Companion version 0.4.0.
- Never `Serial.print` (LOGI/W/E); keys logged as their last 4 characters only.

## Review Focus

- The phone disconnecting mid-turn (BLE drop, app killed): the turn ends with "Lost the phone", the mic stops, the next press picks a path again — never a hang in Listening or Thinking.
- Order of reply audio and JSON from the phone: `turnComplete` must never overtake the last audio frames (both go through one ordered queue on each side).
- A long reply with the iPhone locked: the app's background task must cover the whole reply (begin at `activityEnd`, end at `turnComplete` or close), not only the voice.
- A 60 KB `relay.fetch` body (recall) and its answer in pieces: assembled whole, or failed cleanly after the 30 s timeout — never left half-built for the next message.
- The phone connecting during a Wi-Fi reply: the switch happens at the next press only, never mid-turn.

---

### Task 1: Spike carry-overs (codec, raw BLE, the notify fix)

**Files:**
- Create: `cartridges/companion/adpcm.h` (from the spike), `tools/companion_test/test_adpcm.cpp` (from the spike), `ios/Dotty/Features/Relay/Adpcm.swift` (from the spike), `tools/relay_swift_check/check.swift`
- Modify: `lib/dotty_core/src/core_ble.h/.cpp` (`notifyRaw`, `mtu()`, `notify` retrying a short message), `ios/Dotty/Bluetooth/DottyLink.swift` (Data characteristic, `writeStream`, `maxWriteLength`, routing 0x1F values to `onRelayVoice`… — see Interfaces)

**Interfaces:**
- Produces (C++): `adpcm::State`, `adpcm::kHeader`, `size_t adpcm::encodeFrame(const int16_t*, size_t, State&, uint8_t*)`, `size_t adpcm::decodeFrame(const uint8_t*, size_t, int16_t*, size_t)`; `bool ble::notifyRaw(const uint8_t*, size_t)`, `size_t ble::mtu()`.
- Produces (Swift): `Adpcm.encodeFrame(_: ArraySlice<Int16>, _: inout Adpcm.State) -> Data`, `Adpcm.decodeFrame(_: Data) -> [Int16]`; on `DottyLink`: `func writeStream(_ data: Data) async -> Bool`, `var maxWriteLength: Int`, `var onRelayRaw: ((Data) -> Void)?` (every Event value whose first byte is 0x1F), `var onRelayEvent: ((DottyMessage) -> Void)?` (every event whose name starts with `relay.`).

- [ ] **Step 1:** Bring `adpcm.h` and `test_adpcm.cpp` over from the spike; run `python3 tools/companion_test/run.py adpcm` → PASS, prints `pattern: 00 00 00 70 77 ff 5f`.
- [ ] **Step 2:** Write `tools/relay_swift_check/check.swift` (concatenated with `Adpcm.swift` by `swiftc`) that prints the same pattern for the ramp `[0, 1000, 2000, 3000, -3000, -2000, -1000, 0]`; run it before `Adpcm.swift` exists → compile error; add `Adpcm.swift` → `pattern: 00 00 00 70 77 ff 5f`. Command: `cat ios/Dotty/Features/Relay/Adpcm.swift tools/relay_swift_check/check.swift > $TMP/c.swift && swiftc $TMP/c.swift -o $TMP/c && $TMP/c`.
- [ ] **Step 3:** core_ble: `notifyRaw`, `mtu()`, and `notify` retrying a single short message (up to 50 × 5 ms, logging "notify dropped") like the pieces. `pio run -e companion` → SUCCESS.
- [ ] **Step 4:** DottyLink: discover the Data characteristic; `writeStream` waits for `canSendWriteWithoutResponse` (`peripheralIsReady(toSendWriteWithoutResponse:)`); route 0x1F values to `onRelayRaw` before the 0x1E piece handling; `onRelayEvent` for `relay.*` events. `xcodebuild … build` → BUILD SUCCEEDED.
- [ ] **Step 5:** Commit: `Relay: ADPCM codec (C++ + Swift, byte for byte), raw BLE frames both ways, short notifications retried`.

### Task 2: Relay messages (host-tested)

**Files:**
- Create: `cartridges/companion/relay_messages.h/.cpp`, `tools/companion_test/test_relay_messages.cpp`

**Interfaces:**
- Produces:
  - `class PieceAssembler { bool add(const uint8_t *data, size_t len, std::string &complete); void reset(); }` — feeds a `0x02 flags …` value; returns true with `complete` set when a message finishes; refuses (resets, returns false) beyond `kRelayMaxMessage = 96 * 1024` bytes.
  - `std::string relayOpenEvent(const std::string &setupJson, const std::string &keyHint, const std::string &resume)`, `std::string relaySendEvent(const std::string &message)`, `std::string relayFetchEvent(uint32_t id, const std::string &model, const std::string &body)`, `std::string relayCloseEvent()` — event JSON (`"event": "relay.open"`, `setup` as a string, `resume` only when non-empty).
  - `struct RelayIncoming { enum Kind { Gemini, Fetched, Closed, Bad } kind; std::string gemini; uint32_t id; int status; std::string body; int code; std::string reason; }`, `RelayIncoming parseRelayIncoming(const std::string &json)` — `relay: "fetched"` / `"closed"`, anything else with no `relay` key = a Gemini message (passed through untouched).

- [ ] **Step 1: Failing tests** in `test_relay_messages.cpp`: a 3-piece message assembles exactly; a single-piece message; a piece after an oversized run starts clean; **a 60 KB body in 290-byte pieces assembles whole** (Review Focus); events round-trip through ArduinoJson with `setup`/`message`/`body` as strings and `resume` absent when empty; `parseRelayIncoming` on `{"relay":"fetched","id":7,"status":200,"body":"{…}"}`, `{"relay":"closed","code":1011,"reason":"quota"}`, `{"serverContent":{…}}` (Gemini, untouched) and `{oops` (Bad).
- [ ] **Step 2:** `python3 tools/companion_test/run.py relay_messages` → BUILD FAILED (no `relay_messages.h`).
- [ ] **Step 3:** Implement in `relay_messages.cpp` (ArduinoJson; the assembler's buffer a `std::string` that's reserved once ≥ 4 KB so it lands in PSRAM).
- [ ] **Step 4:** Run → PASS; whole suite `python3 tools/companion_test/run.py` → all files passed.
- [ ] **Step 5:** Commit: `Relay: message framing and events (host-tested)`.

### Task 3: Path choice and grace window (host-tested)

**Files:**
- Create: `cartridges/companion/relay_path.h`, `tools/companion_test/test_relay_path.cpp`

**Interfaces:**
- Produces: `class RelayPath` (plain C++, times in ms):
  - `enum Way { None, Relay, Wifi }`
  - `void press(uint32_t now, bool phoneReady)` — a chat starts (first press of a chat): phone ready → `way() == Relay`; else racing (`racing() == true`, `way() == None`).
  - `void phoneReady(uint32_t now)` — while racing → `Relay` (and `dropWifi()` becomes true once); in a Wi-Fi chat inside the grace window → `handover()` becomes true (taken at the next press).
  - `void wifiReady(uint32_t now)` — while racing → `Wifi`, the grace window starts.
  - `void replyEnded(uint32_t now)` — the first reply's end (for the window).
  - `bool keepBluetooth(uint32_t now) const` — true while racing, in a relay chat, and in a Wi-Fi chat until `max(firstReplyEnd, wifiReadyAt + 15000)` (until the first reply ends, at least 15 s).
  - `bool takeHandover()` — at a press in a Wi-Fi chat: true once if the phone arrived in the window (the caller moves the chat to the relay with the resume handle).
  - `void phoneLost()` — in a relay chat: `way() == None` (the next press chooses again).
  - `void end()` — the chat closed: back to `None`.

- [ ] **Step 1: Failing tests:** phone ready at press → Relay, no racing; phone first while racing → Relay + `dropWifi`; Wi-Fi first → Wifi; window: `keepBluetooth` true at wifiReady+14 s, false at +16 s when the reply ended at +5 s, still true at +20 s when the reply ended at +20 s; phone in the window → `takeHandover()` true once; phone after the window → false; **phone arriving mid-reply → no switch until `takeHandover()` at the next press** (Review Focus); `phoneLost()` in a relay chat → None.
- [ ] **Step 2:** `run.py relay_path` → BUILD FAILED.
- [ ] **Step 3:** Implement `relay_path.h` (header-only, like `key_ring.h`).
- [ ] **Step 4:** Run → PASS; whole suite green.
- [ ] **Step 5:** Commit: `Relay: the path chooser and grace window (host-tested)`.

### Task 4: GeminiLive over a transport

**Files:**
- Create: `cartridges/companion/live_transport.h` (interface + `WsTransport`)
- Modify: `cartridges/companion/gemini_live.h/.cpp`

**Interfaces:**
- Produces:
  ```cpp
  class LiveTransport {
   public:
    virtual ~LiveTransport() = default;
    // Opens the way to Gemini and sends the setup (WebSocket: TLS to the host with the key in the path).
    virtual bool open(const std::string &apiKey, const std::string &setupJson, const std::string &resume, String &error) = 0;
    virtual bool sendText(const std::string &json) = 0;
    virtual bool sendVoice(const int16_t *pcm, size_t frames) = 0;   // WebSocket: liveAudio JSON
    // Messages in arrival order: JSON (Gemini's, audio inline or not) and decoded reply audio.
    virtual bool poll(const std::function<void(const std::string &)> &onJson,
                      const std::function<void(const int16_t *, size_t)> &onAudio) = 0;
    virtual void close() = 0;
    virtual bool connected() const = 0;
    virtual uint16_t closeCode() const = 0;
    virtual const std::string &closeReason() const = 0;
  };
  ```
  `GeminiLive::begin(const std::string &apiKey, const LiveConfig &config, Callbacks callbacks, LiveTransport &transport)`; `const std::string &GeminiLive::latestHandle() const` (the last `newHandle`, for the hand-over); relay audio arrives through `onAudio` and goes through the TurnTracker as a message with content (no tools, no turnComplete).

- [ ] **Step 1:** Refactor with no behaviour change: `WsTransport` wraps `WsClient` (setup sent in `open`), GeminiLive calls the transport everywhere it used `ws_`, `sendRaw` goes through `sendText`. Add `onAudio` handling (TurnTracker `message(true, {}, false, 0)` → deliver → `cb_.audio`).
- [ ] **Step 2:** `python3 tools/companion_test/run.py` → all passed; `pio run -e companion` → SUCCESS.
- [ ] **Step 3: Device check (unchanged Wi-Fi chats):** flash; dev keys `r` (hold) / `r` (release), `U`, `G`, `R` on Debug; log shows `turn:` lines, the writer's reading and the recall as before; `session low` logged.
- [ ] **Step 4:** Commit: `Companion: GeminiLive over a transport (WebSocket unchanged)`.

### Task 5: The relay transport and link on Dotty

**Files:**
- Create: `cartridges/companion/relay_link.h/.cpp`
- Modify: `cartridges/companion/main.cpp` (register `relay.hello`; the link's `poll()` in the loop)

**Interfaces:**
- Consumes: Task 1 (`adpcm`, `ble::notifyRaw`), Task 2 (`PieceAssembler`, events, `parseRelayIncoming`), Task 4 (`LiveTransport`).
- Produces (namespace `relay`):
  - `void begin()` — the PSRAM message buffer (64 KB) fed by `ble::onData` on the BLE task; registers `relay.hello {version, keys}` (stores the hints, `ready` when `version == 1`); clears on BLE disconnect.
  - `bool phoneReady()`, `bool phoneHasKey(const String &keyHint)`
  - `class Transport : public LiveTransport` — `open` sends `relayOpenEvent(setup, last 4 of apiKey, resume)`; `sendText` → `relaySendEvent`; `sendVoice` → ADPCM frames of 570 samples (`0x1F 0x01 seq`, remainder carried to the next call, flushed by `flushVoice()`); `poll` drains the buffer in order (`0x01` → decode → `onAudio`; `0x02` → assembler → `parseRelayIncoming`: Gemini → `onJson`, Closed → close code/reason + `connected() == false`, Fetched → the fetch slot); `close` → `relayCloseEvent`; disconnected when the phone drops ("Lost the phone").
  - `uint32_t fetch(const std::string &model, const std::string &body)` → id; `bool takeFetched(uint32_t id, int &status, std::string &body)`; a fetch older than 30 s reports status 0, body "timed out".

- [ ] **Step 1:** Implement (glue: no host test beyond Tasks 1-3; keep the logic in those).
- [ ] **Step 2:** `pio run -e companion` → SUCCESS; suite green.
- [ ] **Step 3:** Commit: `Companion: the relay transport and link (BLE ⇄ phone)`.

### Task 6: The app's relay service

**Files:**
- Create: `ios/Dotty/Features/Relay/RelayService.swift` (production version; the spike's is the reference), `ios/Dotty/Features/Relay/RelayMessages.swift` (pure: `stripAudio(_ json: Data) -> (json: Data, audio: [Data])`, `pieces(_ json: Data, max: Int) -> [Data]` with the `0x02 flags` header)
- Modify: `ios/Dotty/ContentView.swift` (attach at launch), `ios/Dotty/Bluetooth/DottyLink.swift` (send `relay.hello` once ready), `tools/relay_swift_check/check.swift` (checks for `stripAudio` and `pieces`)

**Interfaces:**
- Consumes: Task 1 (`DottyLink` stream API, `Adpcm`), `GeminiKeys` (Keychain).
- Produces: `RelayService.shared.attach(_ link: DottyLink)`; on `relay.open` → WebSocket to the Live endpoint with the Keychain key whose last 4 match `key` (none → `{relay:"closed", code: 4001, reason: "no such key on this iPhone"}`), the `setup` string sent as-is (with `resume` added to `setup.sessionResumption.handle` when present); `relay.send` → forwarded; voice frames → decoded → `realtimeInput.audio` (100 ms batches); Gemini messages → `stripAudio` → JSON pieces then each audio chunk → ADPCM frames, all on one ordered outbox (Review Focus); WebSocket closed → `{relay:"closed", code, reason}`; `relay.fetch` → POST `https://generativelanguage.googleapis.com/v1beta/models/<model>:generateContent?key=…` (30 s timeout) → `{relay:"fetched", id, status, body}`; `relay.close` → close. Background task: begins at `relay.open` and every voice turn's start, ends at `turnComplete` / close / fetch done (Review Focus).

- [ ] **Step 1:** Failing Swift checks in `check.swift`: `stripAudio` on a message with two `inlineData` parts returns the JSON without them and two audio chunks in order; `pieces` of 700 bytes at max 290 → 3 pieces, flags 1,1,0, payload re-joins exactly. Compile before `RelayMessages.swift` exists → error.
- [ ] **Step 2:** Implement `RelayMessages.swift` → checks print OK.
- [ ] **Step 3:** Implement `RelayService` and the hookups; `xcodebuild … build` → BUILD SUCCEEDED.
- [ ] **Step 4:** Commit: `App: relay service (Gemini through the iPhone, text-model calls for Dotty)`.

### Task 7: Choosing the path on Dotty (race, grace, hand-over, fetch)

**Files:**
- Modify: `cartridges/companion/main.cpp`, `CLAUDE.md`

**Interfaces:**
- Consumes: Tasks 3-6.

- [ ] **Step 1:** `connect()`: `RelayPath::press` with "relay ready" = `relay::phoneReady() && relay::phoneHasKey(keyHint(the session's key))` (a phone without that key = no phone: Wi-Fi); relay ready → `live.begin(…, relayTransport)` (no Wi-Fi, no `ble::stop`); else start the Wi-Fi connect task **without** `ble::stop()` and keep recording; each loop: `relay::phoneReady()` while racing → cancel the Wi-Fi task (`cancelConnect`), begin over the relay with the recording; Wi-Fi ready first → `wifiReady`; `ble::stop()` once `keepBluetooth(now)` turns false; `replyEnded` from `turnDone`; at a press, `takeHandover()` → close the WebSocket, begin over the relay with `live.latestHandle()`; phone lost mid-turn → `setState(Talk::Error, "Lost the phone")`, mic stopped, `phoneLost()`.
- [ ] **Step 2:** The writer / recall / macros job: over the relay, `launchJob` sends `relay::fetch(kWriterModel, body)` instead of the HTTPS task and `writerPoll` takes the result into `jobs` (the existing JobSlot hand-over: start → finish from the loop).
- [ ] **Step 3:** Bump the Companion to `0.4.0`; CLAUDE.md: a "Relay through the iPhone" paragraph (protocol, path choice, measurements); the `CompanionScope` comment ("commands only run while no session") updated — over the relay, commands also run during a chat.
- [ ] **Step 4:** Suite green; `pio run -e companion` → SUCCESS; app builds.
- [ ] **Step 5: Device checks** (log + the app's relay log): relay chat with the app in front and the iPhone locked (voice turns, `G` writer, `R` recall, `D` macro, end-of-chat review, summary); race right after unlocking (phone first → "relay"; phone away, Wi-Fi first → "Wi-Fi"); grace hand-over keeping the conversation (ask "what did I just say?" after the switch); the phone switched off mid-turn → "Lost the phone", next press on Wi-Fi; `session low` during the grace window; the chat screen in the app updating after each relayed reply.
- [ ] **Step 6:** Commit: `Companion 0.4.0: chats through the iPhone over Bluetooth, Wi-Fi raced as the fallback`.

### Task 8: Release

- [ ] **Step 1:** Final whole-branch review (fresh reviewer, most capable model) over the plan's range; fix Critical/Important with tests.
- [ ] **Step 2:** Check outgoing commits for the key; push (HTTPS); `.venv/bin/python tools/build_catalog.py --release`.
