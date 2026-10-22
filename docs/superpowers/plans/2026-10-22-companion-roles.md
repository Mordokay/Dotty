# Companion roles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Up to 8 companions on Dotty, each with its own name (given at its first wake-up), personality, traits, quirks, way of speaking, backstory, voice, avatar or photo, memories and chats; swapped on Dotty or by paging in the app, which can ask Gemini to design one.

**Architecture:** Plain-C++ host-tested parts — `Profile`/`CompanionList` (fields, limits, ids, presets, JSON), `persona.*` (the instruction text, asleep or awake), `PieceBuffer` (long fields and photos sent in pieces) — wrapped by `companions.*` (card folders, active id, migration) which points `memories.*` and `history.*` at the active companion's folder. Avatars regenerated at 128/48 px; Dotty's text area becomes 2 lines. The app's Companion screen becomes a pager with a Gemini co-designer and a photo-avatar editor.

**Tech Stack:** Arduino-ESP32 3.x (PlatformIO `companion` env), ArduinoJson 7, host tests (`python3 tools/companion_test/run.py`), Python + Pillow (`tools/avatar_art.py`), SwiftUI (iOS 26.6, Swift 6), Gemini text API (`generateContent` with a response schema) from the app.

**Spec:** `docs/superpowers/specs/2026-10-22-companion-roles-design.md`

## Global Constraints

- At most 8 companions, at least 1; ids `coach friend partner teacher debug` for the presets, `c<N>` for custom ones, never reused (a `next` counter in `companions/index.json`).
- Limits (Unicode code points, cut at a word on Dotty): name 24, personality 600, traits 6 × 40, quirks 6 × 60, way of speaking 300, backstory 600.
- Card: `data/companions/<id>/{profile.json, memory/memories.json, history/, photo.bin}`; `data/companions/index.json` = `{"next": N, "order": [ids…]}`; NVS `companion/active`.
- Migration (first boot of 0.3.0): `data/memory` and `data/history` → `data/companions/debug/`; the five presets created; active = `debug`. Idempotent (a second run, or a run after a power cut mid-move, ends in the same state).
- Presets: exactly the spec's text for Coach (dog, Puck), Friend (cat, Zephyr), Partner (girl, Achernar), Teacher (owl, Charon), Debug (robot, Zephyr, named "Debug", awake).
- Tool `set_name {name}`: NON_BLOCKING, applied only inside a question (TurnTracker), answered once; name trimmed, ≤ 24, empty ignored.
- Dotty layout: avatar 128 px at (36, 2); text 2 lines below it; the settings screen = nav + 4 rows of 38 px (Companion, Volume, Speak, Voice).
- Photo avatar: 128 and 48 px 1-bit circles (MSB-first rows, 1 = black, a 2 px black ring), 2,048 + 288 bytes, sent base64 in `companion.photo` pieces ≤ 512 bytes per command; `photo.bin` = 128 then 48, written via `.part`.
- The app's Gemini helper uses the key from the Keychain (service `com.greenspherestudios.dotty`, account `gemini-key`), the Flash text model found in Task 9's check, `responseMimeType: application/json` + `responseSchema`; it never suggests names.
- Companion version 0.3.0; commit as papajorgioster@gmail.com; never commit `cartridges/companion/dev_key.h`; memory text in logs ≤ 40 characters; persona text in logs ≤ 40 characters.

## Review Focus

- Switching companion while a session is open (from Dotty's settings or the app) → the session closes; a memory or set_name call that arrives after the switch never lands in the new companion. (Test: Task 4 device check with dev key `S` mid-reply; `applyMemory` uses the session's companion, captured at connect.)
- Deleting the active companion → the next one in order becomes active, its folder is gone, nothing crashes; deleting the last one is refused. (Test: Task 2 `CompanionList` tests.)
- Migration interrupted (power cut between moves) or run twice → same end state, no memory lost. (Test: Task 2 `migrationPlan` host test with partial states; Task 4 device check.)
- A long field or photo sent in pieces and interrupted → the old value stays (pieces are staged, committed only by the last piece). (Test: Task 3 `PieceBuffer` tests.)
- A name or field with accents, emoji or Korean → kept intact in the profile and the app; Dotty's screen shows it through `ui::printable` (an all-foreign name shows "Companion"). (Test: Task 2 round trip; Task 5 screenshot with "Zoë 🌙".)

---

### Task 1: Expressive voice — listening test

**Files:**
- Modify: `cartridges/companion/live_protocol.cpp` (temporary variants), `cartridges/companion/main.cpp` (dev key `V`)

- [ ] **Step 1:** Dev key `V` cycles the next session between: A = today's instruction; B = A + the waking-up text with tone directions ("You have just woken up and can't remember who you are, not even your name. Speak softly and hesitantly, a little lost and sad, not cheerful."); C = B + `"enableAffectiveDialog": true` in the setup (if the server closes with 1007, log the reason and try `enableAffectiveDialogue`). Log the variant at connect.
- [ ] **Step 2:** Flash; for each variant one chat "Hello? Are you awake?" (`say` from the Mac; the user listens too). Expected: A cheerful; B noticeably softer/sadder if prompt steering works; C accepted or refused (logged).
- [ ] **Step 3:** The user judges B vs C. Ledger `Task 1: Ruling: expressive voice = <prompt directions | + affective dialog> — <what was heard / server reply>`. Keep the winner's mechanism (a `LiveConfig::affective` flag if C), remove the dev variants, keep "Let your voice carry your mood." in the base rules. Commit "Companion: expressive voice test".

### Task 2: Profiles and the companion list (host)

**Files:**
- Create: `cartridges/companion/companion_store.h/.cpp` (plain C++ + ArduinoJson)
- Test: `tools/companion_test/test_companion_store.cpp`

**Interfaces:**
- Produces: `struct Profile { std::string id, type, name, personality, speaking, backstory, voice, avatar; std::vector<std::string> traits, quirks; int64_t created = 0; bool awake() const; };` `Profile presetProfile(const std::string &type);` (`coach friend partner teacher debug`) `std::string profileJson(const Profile &, ArduinoJson::Allocator * = nullptr);` `bool parseProfile(const std::string &json, Profile &out);` `std::string cutField(const std::string &, int maxPoints);` `bool setField(Profile &, const std::string &field, const std::string &value);` (fields `name personality speaking backstory voice avatar traits quirks`; traits/quirks as newline-separated text; validates voice via `voiceIndex`, avatar via the 10 ids or `photo`; cuts to the limits). `class CompanionList { public: static constexpr int kMax = 8; bool load(const std::string &indexJson); std::string save() const; const std::vector<std::string> &order() const; std::string create(const std::string &type); /* "" when full; presets keep their id once */ bool remove(const std::string &id, std::string &newActive, const std::string &active); bool has(const std::string &id) const; };` `struct Move { std::string from, to; }; std::vector<Move> migrationPlan(bool oldMemory, bool oldHistory, bool debugMemory, bool debugHistory);`

- [ ] **Step 1: Write the failing tests:** each preset's type, name ("" except Debug = "Debug"), voice and avatar per the spec, `awake()` false except Debug; `profileJson`/`parseProfile` round trip incl. "Zoë 🌙" and a Korean name; `setField` cuts personality at 600 code points at a word, traits to 6 items of ≤ 40, refuses an unknown voice/avatar/field; `CompanionList`: create presets then customs → ids `c1`, `c2`, the 9th → ""; ids never reused after remove; `remove` of the active one → `newActive` = the next in order (or the previous when it was last); removing the only one → false; save/load round trip; `migrationPlan(true, true, false, false)` → 2 moves (`memory`→`companions/debug/memory`, `history`→`companions/debug/history`); with the debug ones already present (a second run) → no moves; half done (`false, true, true, false`) → only the history move.
- [ ] **Step 2: Run** → FAIL (no header). **Step 3: Implement.** **Step 4: Run** → all pass. **Step 5: Commit** "Companion: profiles and the companion list".

### Task 3: Persona, set_name and pieces (host)

**Files:**
- Create: `cartridges/companion/persona.h/.cpp`, `cartridges/companion/piece_buffer.h`
- Modify: `cartridges/companion/live_protocol.h/.cpp` (declare/parse `set_name`; `systemInstruction(time, place, persona, memorySection)` — the persona text replaces the fixed "You are Dotty…" opening), `cartridges/companion/turn_tracker.h` (set_name like a memory call)
- Test: `tools/companion_test/test_persona.cpp`, `test_piece_buffer.cpp`, updates to `test_live_protocol.cpp`, `test_turn_tracker.cpp`

**Interfaces:**
- Produces: `std::string personaText(const Profile &)` — awake: "You are {name}, a small desk companion. {personality} Traits: a, b. Quirks: … How you speak: … Your story: …" (empty fields left out); asleep: the waking-up text (no name; personality underneath; Task 1's winning tone directions; "ask the user who you are; when they tell you your name, call set_name with it, react with relief and become yourself"). `ToolCall` for `set_name` uses `text` = the name. `class PieceBuffer { public: void begin(size_t expected); bool add(size_t offset, const std::string &bytes); /* in order only */ bool complete() const; const std::string &data() const; void clear(); };`

- [ ] **Step 1: Write the failing tests:** awake persona contains the name, personality, "Traits: stubborn, curious." and leaves out an empty backstory; asleep persona has no name, contains "can't remember" and "set_name"; `systemInstruction` with a persona starts with it and still contains the memory guidance and "Let your voice carry your mood"; setup declares `set_name` (NON_BLOCKING, `name` required); a sample `toolCall` with `set_name {name: "Luna"}` parses to `calls[0].text == "Luna"`; TurnTracker applies/answers `set_name` inside a question and not outside; `PieceBuffer`: pieces in order complete exactly at `expected`; an out-of-order piece → false and not complete; `clear()` drops a half buffer.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → all pass; firmware build SUCCESS. **Step 5: Commit** "Companion: personas, set_name, pieces".

### Task 4: Companions on Dotty

**Files:**
- Create: `cartridges/companion/companions.h/.cpp`
- Modify: `cartridges/companion/memories.h/.cpp` and `history.h/.cpp` (a folder parameter instead of the fixed `data/memory`, `data/history`), `cartridges/companion/main.cpp`

**Interfaces:**
- Consumes: Tasks 2-3.
- Produces: `namespace companions { void begin(); /* migration + load */ const Profile &active(); bool select(const std::string &id); bool save(const Profile &); const CompanionList &list(); bool create(const std::string &type, std::string &id); bool remove(const std::string &id); String folder(const std::string &id); }`; `memories::begin(const String &folder)`, `history::setFolder(const String &)`.

- [ ] **Step 1: Implement** `companions.*` (migration from `migrationPlan`, presets created when `index.json` is missing, active from NVS), memories/history per folder (switching calls `memories::begin(folder)` + `history::setFolder`). In `main.cpp`: the session's companion id captured in `connect()` (`sessionCompanion`); `applyMemory` and `set_name` only act when `sessionCompanion == companions::active().id`; voice and avatar from the active profile (settings' voice row edits it); `set_name` → `setField(name)`, save, note "Named: {name}" (via `ui::printable`); asleep → status "Waking up…" under the avatar; switching closes the session (`disconnect("companion changed")`); the lock widget shows "{name} · N chats today" (asleep: "Not awake yet"); dev keys `S` = select the next companion, `W` = clear the active one's name (asleep again).
- [ ] **Step 2: Device check:** first boot → log "migrated: 2 moves", five companions, active debug with its 7+ memories; second boot → "migrated: 0 moves"; `S` to Friend → chat → it wakes up lost, you say "Your name is Luna" → `set_name` → "Named: Luna"; new chat → it's Luna, and asking about the guitar (a Debug memory) → it doesn't know; `S` back to Debug → it knows; `S` mid-reply → session closes, no memory written to the new one.
- [ ] **Step 3: Commit** "Companion: companions on Dotty".

### Task 5: 128 px avatars, 2-line layout, photo avatars on Dotty

**Files:**
- Modify: `tools/avatar_art.py` (`BIG = 128`; the face kit scales from 96), `cartridges/companion/avatar.h/.cpp` (`kBig = 128`; draws a photo + mood badge when the avatar is `photo`), `cartridges/companion/screens.cpp` (layout), `cartridges/companion/main.cpp` (the photo loaded from `photo.bin` into PSRAM on select)

- [ ] **Step 1:** Regenerate the art at 128/48 and check `tools/avatar_art_preview.png` by eye (faces placed right on all ten; tune `FACE` if not). Implement the layout per Global Constraints: 2-line text, meter between avatar and text, the note overlaid on the avatar's bottom edge, settings with four 38 px rows and the Companion row first, and photo avatars drawn as the stored bitmap with a 22 px mood badge on the lower right (icons drawn in code).
- [ ] **Step 2: Device check (screenshots):** Ready, Listening (meter), Thinking, Speaking (page "1/3"), After, Error, text-only, settings, lock screen, an asleep companion ("Waking up…"), a companion named "Zoë 🌙" (printable), a photo avatar (dev key `P` writes a test circle to `photo.bin`). Firmware size ≤ 3.8 MB.
- [ ] **Step 3: Commit** "Companion: bigger avatars, 2-line text, photo avatars".

### Task 6: BLE commands

**Files:**
- Modify: `cartridges/companion/main.cpp` (`registerCommands`)

- [ ] **Step 1: Implement** the spec's commands: `companion.list`, `companion.get {id}`, `companion.select {id}`, `companion.create {type}` (+ any fields), `companion.update {id, field: value…}` (short fields), `companion.field {id, field, offset, text, total}` (long fields via `PieceBuffer`, committed when complete), `companion.delete {id}`, `companion.photo {id, size, offset, data, total}` (base64; both sizes complete → `photo.bin` via `.part`, avatar = `photo`), `companion.photo.clear {id}`; `companion` parameter on `companion.memories`, `memory.*`, `history`, `forget` (default active); `companion.status.active` + `.name`. Every change notifies `companion.changed`.
- [ ] **Step 2:** Build SUCCESS; commit "Companion: companion commands". (Checked through the app in Tasks 7-9.)

### Task 7: The app's pager

**Files:**
- Create: `ios/Dotty/Features/Companion/CompanionPager.swift` (pager + page), `CompanionSettingsSheet.swift` (key, volume, Speak, today), `Keychain.swift` (`Features/Common/`)
- Modify: `CompanionView.swift` (becomes the pager's host), `DashboardView.swift` route unchanged

- [ ] **Step 1: Implement:** `TabView(.page)` of companions from `companion.list` + a "+" page while < 8; each page: avatar/photo, name or "Not awake yet", "On Dotty" badge, voice (VoicesView), avatar grid (+ "Your photo"), the four fields (TextEditor with counters; long fields sent with `companion.field`), Memories (`MemoriesView(companion:)`), Past chats (`CompanionHistoryView(companion:)`), Delete (confirmation; disabled for the last). Settling on a page 0.5 s → `companion.select` when connected. The gear sheet holds the key field (saves to the Keychain and sends to Dotty), volume, Speak, today's usage.
- [ ] **Step 2: Build** `xcodebuild -project ios/Dotty.xcodeproj -scheme Dotty -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build` → BUILD SUCCEEDED. Commit "App: companion pager".

### Task 8: Photo avatars in the app

**Files:**
- Modify: `ios/Dotty/Features/Album/PhotoDither.swift` (`grey`/`dither` take a `size`, default 200)
- Create: `ios/Dotty/Features/Companion/AvatarPhotoEditor.swift`

- [ ] **Step 1: Implement:** pick a photo → circular crop (pinch/drag, brightness/contrast, live preview at 128 and 48) → dither at 128 and 48 → circle mask + 2 px ring → upload both with `companion.photo` pieces (progress shown) → avatar = photo. "Remove photo" goes back to the chosen avatar.
- [ ] **Step 2: Build** → BUILD SUCCEEDED. Commit "App: photo avatars".

### Task 9: The Gemini co-designer

**Files:**
- Create: `ios/Dotty/Features/Companion/CompanionDesigner.swift` (+ page and Edit), `ios/Dotty/Features/Companion/GeminiText.swift`

- [ ] **Step 1: Check the model:** `GET https://generativelanguage.googleapis.com/v1beta/models?key=…` (from the Mac with the bench key; don't print the key) → pick the newest `flash` model with `generateContent`; ledger it.
- [ ] **Step 2: Implement** `GeminiText.generate(prompt, schema) async throws -> [String: Any]` (POST `:generateContent?key=` with `generationConfig.responseMimeType = application/json` + `responseSchema`); the designer: "Describe your companion" → one call returning personality, traits, quirks, speaking, backstory, voice (one of the 30) + reason, avatar (one of the 10) → editable form → Create (`companion.create {type: custom}` + fields) or Save; ✨ Suggest per field (a call with the description and the other fields). Errors (no key, quota, offline) leave the fields as they were with a message.
- [ ] **Step 3: Build** → BUILD SUCCEEDED; **check from the Mac** with the same request body (curl, key from `dev_key.h` via an env var, not printed): a description returns JSON matching the schema. Commit "App: Gemini co-designer".

### Task 10: Ship

- [ ] **Step 1:** `DOTTY_CARTRIDGE(..., "0.3.0")`; remove dev variants left from Task 1; CLAUDE.md (Companion: roles — fields, presets, onboarding, layout, photo, BLE, app); cartridge.json description ("…a few companions, each their own person, who remember you").
- [ ] **Step 2:** The user's pass in the app (pager, + with Gemini, a photo avatar, switching). Host tests pass; firmware + app build.
- [ ] **Step 3:** Commit, key check on outgoing commits, push, `.venv/bin/python tools/build_catalog.py --release` → companion 0.3.0.
