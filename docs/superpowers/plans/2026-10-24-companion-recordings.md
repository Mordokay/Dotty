# Companion Recordings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Voice notes recorded through a companion ("record something about bananas in space"): kept on Dotty and copied to the iPhone while you speak, transcribed (Apple's live draft, Gemini's final), summarised with points, reacted to, playable by voice, known to the companion afterwards; plus the app's avatar pager and Dotty's timer button.

**Architecture:** Host-tested plain C++ holds the rules (the recordings store, the deck's state machine, the `rec.*` messages, the tools); `voice_note.*` records the companion's 16 kHz capture to a WAV on the card and as ADPCM frames to the phone over the relay link; `main.cpp` runs the deck, the reaction, the tools and a `sync` phone-wake job. The app's `RecordingService` (app-wide, like `RelayService`) keeps the WAV, runs Apple's on-device recognition live, then Gemini's final transcript and the summary, and sends the text back.

**Tech Stack:** ESP32-S3 Arduino (pioarduino, the companion's custom core), NimBLE-Arduino 2.x, ArduinoJson 7, FreeRTOS tasks; SwiftUI, CoreBluetooth, Speech (`SFSpeechRecognizer`), AVFoundation (Swift 6).

**Spec:** `docs/superpowers/specs/2026-10-24-companion-recordings-design.md`

## Global Constraints

- 16 kHz, 16-bit mono WAV; 30 minutes at most (a beep at 29:30, Done at 30:00); nothing recorded within 2 minutes → closed, nothing kept; 5 minutes paused untouched → Done; locking = Done; < 50 MB free on the card → the deck doesn't open.
- Files: `data/companions/<id>/recordings/<YYYYMMDD-HHMMSS>.wav`, `.json` {title, created, ms, parts, synced, textAt, isDraft, summary, points[]}, `.txt` (the final transcript, else the draft). Never deleted automatically.
- Frames to the phone: `0x1F 0x02 seq` + an ADPCM frame (16 kHz, `relayVoiceSamples(mtu)` samples, own start state); events `rec.begin {id, companion, title}`, `rec.part {id, n}`, `rec.undo {id}`, `rec.end {id, how: done|discard}`; the phone asks `rec.fetch {id}` for a whole recording.
- Back to Dotty: `companion.recording.text {id, companion, transcript, isDraft, summary, points[], title?, draft?}`; long fields in 240-byte pieces `companion.recording.piece {id, field, offset, total, text}`.
- Final transcript: recordings ≤ 10 minutes → the audio to `kWriterModel` with the draft, memories and title as hints ("transcribe exactly what was said; keep their words; fix only mishearings"); longer → the draft corrected without audio. Summary: 1-2 sentences + 3-6 points (+ a title when the given one is a placeholder).
- In every chat: the latest 10 recordings' summaries (date, title, summary) next to the chat summaries; recall also searches transcripts.
- Tools: `start_recording {title}`, `play_recording {query}`, `delete_recording {query}` (only after the user confirmed in the chat).
- Sandbox: recordings only in the companion's own folder; Tape untouched.
- Companion version 0.6.0. LOGI/W/E only; keys only as their last 4 characters.
- Every cartridge change ends with commit → push → catalog release.
- For the testing phase (the user, 2026-10-24): the phone also sends Apple's draft (`draft`) with the final text and Dotty **logs both** on arrival (not stored), so Claude can read and compare them; the app shows both (Final / Apple tabs).

## Review Focus

- The phone dropping (or locking) mid-recording: the card copy is complete, the deck never stalls, and the sync job later sends the whole file. Test: `test_note_deck` "phone gone mid-recording", device step in Task 7.
- A recording ended by the auto-lock or a power cut: everything up to the last pause survives, the header is repaired at boot, the companion doesn't react to a half-made transcript. Test: `test_recording_store` "header repair plan", device step in Task 6.
- Undo while frames are still in flight to the phone: the phone's copy drops exactly the last part. Test: Swift check "undo after late frames" (Task 10).
- Two recordings in a row before the first's text came back: texts land on the right recording (by id), never the newest. Test: `test_rec_messages` "text for an older id".
- A long transcript (30 minutes ≈ 27 KB) arriving in pieces with one lost: nothing half-written lands in `.txt`; the phone retries. Test: `test_rec_messages` "pieces with a gap".

---

### Task 1: The background speech probe (iOS)

**Files:**
- Modify: `ios/Dotty/Features/Relay/RelayLogView.swift` (a "Background speech test" button), `ios/Dotty-Info.plist` (`NSSpeechRecognitionUsageDescription`: "Dotty turns your voice notes into text on this iPhone.")
- Create: `ios/Dotty/Features/Recordings/SpeechProbe.swift`, `ios/Dotty/Resources/speech_probe.wav` (made with `say -o probe.aiff "…20 seconds of speech…" && afconvert -f WAVE -d LEI16@16000 probe.aiff speech_probe.wav`)

**Interfaces:**
- Produces: `SpeechProbe.run() async -> String` (a report line: authorization, `supportsOnDeviceRecognition`, app state at each partial result, final text, words recognized).

- [ ] **Step 1:** `SpeechProbe` requests authorization, then feeds the bundled WAV to an `SFSpeechAudioBufferRecognitionRequest` (`requiresOnDeviceRecognition = true`, `shouldReportPartialResults = true`) in 100 ms buffers every 100 ms (a live stream), inside a background task, logging `UIApplication.shared.applicationState` with each partial result to the Relay log.
- [ ] **Step 2:** Build: `xcodebuild -project ios/Dotty.xcodeproj -scheme Dotty -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build` → BUILD SUCCEEDED.
- [ ] **Step 3:** The user runs it on the iPhone: tap the test, lock the phone at once, unlock after 30 s, read the log. Record the answer in the ledger: **live on-device recognition works in the background: yes / no**. No → Task 10 makes the draft from the file once the app is active (spec's fallback).
- [ ] **Step 4:** Commit: `App: background speech probe (Relay log)`.

### Task 2: The recordings store (host-tested)

**Files:**
- Create: `cartridges/companion/recording_store.h/.cpp`, `tools/companion_test/test_recording_store.cpp` (`// uses: cartridges/companion/recording_store.cpp cartridges/companion/moment_store.cpp`)

**Interfaces:**
- Produces:
  ```cpp
  struct RecordingMeta {
    std::string name;   // "20261024-153012" (no extension)
    std::string title;
    int64_t created = 0; uint32_t ms = 0; int parts = 0;
    bool synced = false, isDraft = false; int64_t textAt = 0;  // textAt 0 = no text yet
    std::string summary; std::vector<std::string> points;
  };
  std::string recordingName(int64_t localEpoch);                       // "20261024-153012"
  bool parseRecordingMeta(const std::string &json, RecordingMeta &out);
  std::string recordingMetaJson(const RecordingMeta &m);
  // Newest first; "the last one", a date ("yesterday", "24 October", "2026-10-24") or title / summary words.
  int matchRecording(const std::vector<RecordingMeta> &newestFirst, const std::string &query, int64_t now);  // index, -1 none
  std::string recordingsSection(const std::vector<RecordingMeta> &newestFirst, int max);  // instruction; "" when none
  // recall: a recording's transcript as dated lines ("Sat 24 Oct 15:30 - Recording 'Bananas in space': …").
  std::string transcriptBlock(const RecordingMeta &m, const std::string &transcript);
  struct HeaderFix { bool needed; uint32_t dataBytes; };
  HeaderFix headerFix(uint32_t fileBytes, uint32_t headerDataBytes);  // a WAV cut short by a power cut
  ```

- [ ] **Step 1: Write the failing test** `test_recording_store.cpp`: "recordingName"; "meta round trip" (every field, 4 points); "broken meta is false"; "match: the last one" (index 0); "match: a title word" ("bananas" → the Bananas one, case-insensitive); "match: a date" ("2026-10-22" → that day's newest; "yesterday" from now); "match: a summary word"; "match: nothing → -1"; "section: newest 10, with date and title, no points"; "section: empty → empty"; "transcriptBlock has the date and title"; "header repair plan" (file 44 + 32000 bytes, header says 16000 → needed, 32000; file shorter than 44 → not needed, 0; odd byte count → rounded down to whole samples).
- [ ] **Step 2:** `python3 tools/companion_test/run.py recording_store` → BUILD FAILED.
- [ ] **Step 3:** Implement (ArduinoJson; dates from `gmtime_r` on the local epoch; words lower-cased, ≥ 3 letters).
- [ ] **Step 4:** Run → PASS; whole suite passes.
- [ ] **Step 5:** Commit: `Companion: the recordings store (host-tested)`.

### Task 3: The recording deck's rules (host-tested)

**Files:**
- Create: `cartridges/companion/note_deck.h` (header-only), `tools/companion_test/test_note_deck.cpp`

**Interfaces:**
- Produces:
  ```cpp
  class NoteDeck {
   public:
    static constexpr uint32_t kIdleCloseMs = 120000, kPausedDoneMs = 300000, kMaxMs = 1800000, kWarnMs = 1770000;
    enum class State { Closed, Waiting, Recording, Paused, ConfirmDiscard };
    enum class Event { None, Begin, PartStart, PartEnd, Warn, Done, Discard, CloseEmpty };
    void open(uint32_t now);                 // → Waiting
    Event hold(bool down, uint32_t now);     // BOOT down/up: PartStart / PartEnd (Waiting/Paused ↔ Recording)
    Event undo();                            // Paused, parts > 0: drops the last part's length
    Event done();                            // Paused (parts > 0) → Done; nothing recorded → CloseEmpty
    Event discard(bool confirmed);           // first call → ConfirmDiscard (if parts), confirmed → Discard
    Event cancel();                          // back arrow: nothing recorded → CloseEmpty, else ConfirmDiscard
    Event lockOrPower();                     // = done() (recording → paused first)
    Event tick(uint32_t now);                // the limits: CloseEmpty at 2 min waiting, Done at 5 min paused, Warn at 29:30, Done at 30:00
    State state() const; uint32_t elapsedMs(uint32_t now) const; int parts() const; uint32_t lastPartMs() const;
  };
  ```

- [ ] **Step 1: Write the failing test** `test_note_deck.cpp`: "hold records, release pauses, parts count"; "elapsed counts only recording"; "undo drops the last part"; "undo with nothing: None"; "done with nothing → CloseEmpty"; "done → Done"; "discard asks first, then Discard"; "cancel with nothing closes, with something asks"; "2 minutes waiting → CloseEmpty"; "5 minutes paused → Done"; "warn at 29:30 once, Done at 30:00 (recording)"; "lock while recording → Done"; "phone gone mid-recording" (no event: the deck doesn't care — Recording continues).
- [ ] **Step 2:** `python3 tools/companion_test/run.py note_deck` → BUILD FAILED.
- [ ] **Step 3:** Implement `note_deck.h`.
- [ ] **Step 4:** Run → PASS; whole suite passes.
- [ ] **Step 5:** Commit: `Companion: the recording deck's rules (host-tested)`.

### Task 4: rec messages and the returned text (host-tested)

**Files:**
- Create: `cartridges/companion/rec_messages.h/.cpp`, `tools/companion_test/test_rec_messages.cpp` (uses `piece_buffer.h`, `recording_store.cpp`)

**Interfaces:**
- Produces: `std::string recBeginEvent(const std::string &id, const std::string &companion, const std::string &title)`, `recPartEvent(id, int n)`, `recUndoEvent(id)`, `recEndEvent(id, bool done)`; `size_t recFrameHeader(uint8_t *out, uint8_t seq)` (writes `0x1F 0x02 seq`, returns 3); `struct RecText { std::string id, companion, transcript, draft, summary, title; std::vector<std::string> points; bool isDraft; }`; `class RecTextAssembler { bool command(JsonObjectConst args, RecText &complete); bool piece(JsonObjectConst args); }` — the text command may arrive with `"transcript": "..."` (short) or `"pieces": true` (the long fields follow as pieces, completed by a second `companion.recording.text {id, done: true}`).

- [ ] **Step 1: Write the failing test** `test_rec_messages.cpp`: "events carry their fields" (each of the four, parsed back); "frame header"; "short text in one command"; "long transcript in pieces"; "pieces with a gap → not complete, nothing returned"; "text for an older id" (two ids interleaved → each completes with its own text).
- [ ] **Step 2:** `python3 tools/companion_test/run.py rec_messages` → BUILD FAILED.
- [ ] **Step 3:** Implement (reuse `PieceBuffer` per id and field).
- [ ] **Step 4:** Run → PASS; whole suite passes.
- [ ] **Step 5:** Commit: `Companion: rec.* messages and the returned transcript (host-tested)`.

### Task 5: The recording tools (host-tested)

**Files:**
- Modify: `cartridges/companion/live_protocol.cpp` (declarations, instruction rules), `cartridges/companion/gemini_live.cpp` (the three go through `cb.tool` like the moment tools), `cartridges/companion/turn_tracker.h` (none of them auto-answered except start_recording)
- Test: `tools/companion_test/test_live_protocol.cpp`, `test_turn_tracker.cpp`

**Interfaces:**
- Produces: tools `start_recording {title}`, `play_recording {query}`, `delete_recording {query}` (query travels as `text`, title as `label`); instruction: "When the user wants to record something (a voice note, an idea, a story), call start_recording with a short title and tell them to start whenever they're ready; the recording itself never comes to you, only its text afterwards. To play one, call play_recording; to delete one, only after they confirmed, delete_recording."

- [ ] **Step 1: Write the failing test**: "setup declares start_recording, play_recording, delete_recording"; "parse start_recording's title"; "instruction: recordings"; turn tracker "play_recording applied, not answered".
- [ ] **Step 2:** Run `python3 tools/companion_test/run.py live_protocol turn_tracker` → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run → PASS; whole suite passes; `pio run -e companion` → SUCCESS.
- [ ] **Step 5:** Commit: `Companion: start_recording, play_recording, delete_recording`.

### Task 6: The recorder on Dotty (`voice_note.*`)

**Files:**
- Create: `cartridges/companion/voice_note.h/.cpp`
- Modify: `cartridges/companion/relay_link.h/.cpp` (`bool relay::sendRecordingFrame(const int16_t *pcm, size_t n)`, its own seq and encoder state), `cartridges/companion/main.cpp` (boot repair)

**Interfaces:**
- Consumes: Task 2's `headerFix`, `recordingName`, `RecordingMeta`; Task 4's events and header.
- Produces (`namespace note`): `bool begin(const std::string &companion, const std::string &title)` (creates the WAV with a 44-byte header, the .json; sends `rec.begin` if the phone is ready); `void add(const int16_t *pcm, size_t n)` (to the writer task's PSRAM stream buffer and, while recording a part, to the phone); `void partStart()`, `void partEnd()` (writes the header length), `uint32_t undo()` (VFS `truncate` to the part's start; `rec.undo`), `std::string finish(bool keep)` (closes; discard deletes; `rec.end`), `void repairAll()` (boot: every companion's recordings with `headerFix` needed), `const RecordingMeta &current()`, `bool streaming()`.

- [ ] **Step 1:** Implement: a writer task (core 1, 6 KB stack) moves a 64 KB PSRAM stream buffer to the card (Tape's pattern, card writes through a 4 KB internal chunk); frames to the phone only while `relay::phoneReady()` (a drop stops streaming for this recording; `synced` stays false).
- [ ] **Step 2:** Boot: `note::repairAll()` after `moments::begin()`, logging each fix.
- [ ] **Step 3:** Build → SUCCESS. Device: dev key `n` toggles a 10 s test recording for the active companion (hold simulated); `w` hex-dumps the newest over serial (`#WAV <size> <name>` … `#END`, Tape's format) → the Mac plays it (`afplay`). Power-cut test: start a recording, pull the battery mid-hold, boot → "repaired …" and the file plays up to the last pause.
- [ ] **Step 4:** Commit: `Companion: the voice-note recorder (card + phone)`.

### Task 7: The deck, the reaction, the tools on Dotty

**Files:**
- Modify: `cartridges/companion/main.cpp`, `cartridges/companion/screens.h/.cpp`

**Interfaces:**
- Consumes: Tasks 3-6.
- Produces: `screens::drawDeck(const DeckView &v)` (`struct DeckView { String title, clock; bool recording, paused, confirm, empty; int levelDb; };`), `DeckHit screens::deckHit(x, y)` (`enum class DeckHit { None, Back, Undo, Done, Discard, Yes, No }`); main.cpp `NoteDeck deck`, `void openDeck(const std::string &title)`, `void pollDeck(const shell::Input &)`, `void reactToRecording(const RecordingMeta &, const std::string &transcript)`.

- [ ] **Step 1:** `start_recording` (tool) → `{ok}`; once the reply ends (state After, not speaking) → `openDeck`; < 50 MB free → the tool answers `{error: "no room on the card"}` (spoken) instead.
- [ ] **Step 2:** The deck: BOOT holds go to `note::add` (not Gemini: `voice::nextChunk` routed by the deck), the meter, the buttons and the confirm row, back = cancel, `deck.tick` each pass, `shell::wake()` while open, lock → `lockOrPower()`; Warn → a beep; the session may idle-close meanwhile (no review while the deck is open).
- [ ] **Step 3:** Done → `note::finish(true)`; with the phone streaming, wait ≤ 10 s for `companion.recording.text` (the draft), then `reactToRecording`: a typed note through the chat path (reconnecting if needed): "(Dotty, not the user:) They just recorded '<title>' (m:ss). Transcript: <text>. Say it's saved and react to what they said, briefly, in character; save lasting facts with remember." Without text: "… you can't hear it yet: say it's saved and you'll listen properly once their phone is near." History line `(recording) <title> · m:ss`.
- [ ] **Step 4:** `play_recording` → `matchRecording` → `player.play(wav)` with the screen showing the title (speech badge), result `{playing: title}` spoken; `delete_recording` → delete wav/json/txt, `{deleted: title}`.
- [ ] **Step 5:** Build → SUCCESS. Device: "I want to record something about bananas in space" → the deck; two holds, undo, a third, Done → reaction within ~10 s naming what was said; "play my bananas recording" plays it; cancel with and without a recording; lock while paused = Done. **Phone gone mid-recording** (turn the iPhone's Bluetooth off during a hold): the deck goes on, the card copy plays whole.
- [ ] **Step 6:** Commit: `Companion: the recording deck, the reaction, play and delete by voice`.

### Task 8: What the companion knows; commands; the sync job

**Files:**
- Modify: `cartridges/companion/main.cpp`

**Interfaces:**
- Produces: BLE `companion.recordings {companion}` → {recordings[{name, title, created, ms, synced, text: textAt > 0, isDraft, summary}]}, `companion.recording.text`, `.piece`, `.rename {companion, name, title}`, `.delete {companion, name}`, `.play {companion, name}`; event `companion.recordings`; phone-wake job **sync** (due while any recording has `!synced` or no text and the phone is reachable; Wi-Fi can't do it); handling of `rec.fetch {id}` from the phone (the whole WAV as `0x02` frames, then `rec.end {done}`).

- [ ] **Step 1:** `connect()`: `recordingsSection(…, 10)` after the chat summaries. Recall: the days asked for also read that companion's recordings made those days (`transcriptBlock` per recording, into `recallBlock`'s day text).
- [ ] **Step 2:** The commands; `companion.recording.text` → `RecTextAssembler` → write `.txt` (via `.part`), update `.json` (summary, points, title, isDraft, textAt, synced), log `transcript (Apple): …` and `transcript (final): …` lines (the testing phase), notify; a reaction waiting for this id gets it.
- [ ] **Step 3:** The sync job and `rec.fetch`.
- [ ] **Step 4:** Build → SUCCESS; `python3 tools/companion_test/run.py` → all pass. Device: record with the iPhone's Bluetooth off → "Waiting to sync"; turn it on, unlock → the phone fetches it, the text comes back; "what did I say in my bananas recording about the moon?" (recall) quotes the line.
- [ ] **Step 5:** Commit: `Companion: recordings in the instruction and recall, commands, the sync job`.

### Task 9: Dotty's timer button

**Files:**
- Modify: `cartridges/companion/main.cpp`, `cartridges/companion/screens.h/.cpp`

**Interfaces:**
- Produces: `ScreenInfo::timerChip` (String, "7:12", "" = none) drawn as a small hourglass + time in the top-right corner instead of the history clock; `timerOpen` (bool) — the timer view only when open; the timer view gets a nav bar with a back arrow (`TimerHit::Back`).

- [ ] **Step 1:** Tap on the chip → `timerOpen = true`; back → false; in the last minute `timerOpen = true` by itself (Dotty unlocks, as now); the history corner moves under the chip while a timer runs (history stays reachable).
- [ ] **Step 2:** Build → SUCCESS; device: start a 2-minute timer, the chip counts, open / back, the last minute opens it.
- [ ] **Step 3:** Commit: `Companion: a timer button on the main screen; the timer screen has a back arrow`.

### Task 10: The iPhone's RecordingService

**Files:**
- Create: `ios/Dotty/Features/Recordings/RecordingService.swift`, `ios/Dotty/Features/Recordings/RecordingStore.swift` (the local files + `index.json`), `tools/relay_swift_check/recording_check.swift`
- Modify: `ios/Dotty/Bluetooth/DottyLink.swift` (route `0x1F 0x02` values to `onRecordingRaw`, `rec.*` events to `onRecordingEvent`), `ios/Dotty/ContentView.swift` (attach it), `ios/Dotty/Features/Companion/GeminiText.swift` (`generateWithAudio(_ prompt: String, wav: Data, schema:) async throws -> [String: Any]` — an `inline_data` part, mime `audio/wav`)

**Interfaces:**
- Produces: `RecordingService.attach(_ link: DottyLink)`; `RecordingStore.recordings(companion:) -> [LocalRecording]`, `LocalRecording { id, companion, title, created, duration, wavURL, draft, final, summary, points, synced }`.

- [ ] **Step 1: Write the failing Swift check** `recording_check.swift` (with `Adpcm.swift` and the parts logic): "frames in order build the parts"; "a gap in seq is found (→ rec.fetch)"; "undo after late frames drops exactly the last part". Run: `cat ios/Dotty/Features/Relay/Adpcm.swift ios/Dotty/Features/Recordings/RecordingParts.swift tools/relay_swift_check/recording_check.swift > $TMP/r.swift && swiftc $TMP/r.swift -o $TMP/r && $TMP/r` → fails (no `RecordingParts`).
- [ ] **Step 2:** `RecordingParts.swift` (pure: parts of PCM, seq tracking, undo) → the check passes.
- [ ] **Step 3:** The service: `rec.begin` → a new local recording (background task until `rec.end`); frames → parts + the live `SFSpeechAudioBufferRecognitionRequest` (Task 1's answer decides live vs after); `rec.end done` → WAV saved; then **final transcript** (≤ 10 minutes: `generateWithAudio`; else a correction of the draft), the **summary** (`{summary, points[3-6], title?}`), and `companion.recording.text` (pieces when > 240 bytes) with `draft` included (testing phase); quota error → `isDraft: true`, retried the next day; a gap → `rec.fetch {id}`.
- [ ] **Step 4:** Build → BUILD SUCCEEDED.
- [ ] **Step 5:** Commit: `App: RecordingService — streaming, Apple's draft, Gemini's final transcript, summary`.

### Task 11: The app's Recordings screens and the avatar pager

**Files:**
- Create: `ios/Dotty/Features/Recordings/RecordingsView.swift`, `ios/Dotty/Features/Recordings/RecordingDetailView.swift`
- Modify: `ios/Dotty/Features/Companion/CompanionView.swift` (a Recordings row on each page; the pager)

**Interfaces:**
- Produces: `RecordingsView(companion:)`; `CompanionPager` (the avatar circles row: 28 px, rings in `DottyLight.allCases[index % 7]`, full ring on the page shown, dimmed others, a dot under the one active on Dotty, a dashed "+"; tap = jump).

- [ ] **Step 1:** List (merging Dotty's `companion.recordings` with the local store): title, date, length, summary, "On iPhone ✓" / "Waiting to sync", "draft".
- [ ] **Step 2:** Detail: play (AVAudioPlayer), summary + points, transcript with **Final / Apple** tabs (selectable), share (audio, text), rename, delete (both, confirmation), Play on Dotty.
- [ ] **Step 3:** The pager replaces `.page(indexDisplayMode: .always)` with `.never` + `CompanionPager` above or below the pages.
- [ ] **Step 4:** Build → BUILD SUCCEEDED.
- [ ] **Step 5:** Commit: `App: Recordings list and page (Final / Apple transcripts); the companion pager as avatar circles`.

### Task 12: The transcript comparison, docs, version, release

**Files:**
- Modify: `cartridges/companion/main.cpp` (`DOTTY_CARTRIDGE("companion", "Companion", "0.6.0")`), `CLAUDE.md` (a **Recordings** bullet in the Companion section, the probe's answer, the pager, the timer button), the memory file `dotty-ai-companion.md` (3b shipped)

- [ ] **Step 1:** The user makes two or three 1-2 minute recordings in their own voice; Claude reads the `transcript (Apple)` / `transcript (final)` log lines and reports: words Apple got wrong, whether Gemini fixed them, any Gemini changes that weren't mishearings — a recommendation on keeping Apple's draft in the loop.
- [ ] **Step 2:** `python3 tools/companion_test/run.py` → all pass; `pio run -e companion` → SUCCESS; the app build → BUILD SUCCEEDED.
- [ ] **Step 3:** Commit, check outgoing commits for a full key (`grep -cE "AIza[0-9A-Za-z_-]{35}"` → 0), push, `.venv/bin/python tools/build_catalog.py --release`.
