# Companion memories Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The companion keeps up to 300 short memories on the SD card, gets them all at the start of every chat, saves/updates/forgets them by itself through function calls, and the app lists, searches, adds, edits and deletes them.

**Architecture:** A plain-C++ `MemoryStore` (host-tested: JSON in/out, add/update/remove/cap/dedupe, the prompt section) wrapped by `memories.*` for the card. `live_protocol` declares three NON_BLOCKING tools and parses every call of a `toolCall` message; `TurnTracker` decides per call whether it's applied and answered; `main.cpp` applies them, shows a note, logs them in history. Six BLE commands + `MemoriesView` in the app.

**Tech Stack:** Arduino-ESP32 3.x (PlatformIO `companion` env), ArduinoJson 7, host tests with clang++ (`python3 tools/companion_test/run.py`), SwiftUI (iOS 26.6, Swift 6).

**Spec:** `docs/superpowers/specs/2026-10-22-companion-memories-design.md`

## Global Constraints

- At most 300 memories; text at most 200 characters (UTF-8 bytes counted as characters is NOT acceptable: count Unicode code points; cut at a word boundary).
- Groups, in this order and with these labels: `you` "About you", `people` "People", `likes` "Likes & dislikes", `plans` "Plans & dates", `other` "Other".
- File `/cartridges/companion/data/memory/memories.json` = `{"next": N, "memories": [{"id", "group", "text", "by", "added", "changed"}]}`, written via `memories.json.part` + rename; a broken file is renamed `memories.bad.json` and an empty store started.
- Tools `remember {text, group}`, `update_memory {id, text, group?}`, `forget_memory {id}`, all `"behavior": "NON_BLOCKING"`.
- A memory call is applied only while a question is open (TurnTracker), never from an unasked or draining turn.
- Note on Dotty for ~3 s: "Remembered" / "Updated" / "Forgotten" / "Memory full", with a bookmark icon, under the avatar.
- BLE commands only while no session (as in piece 1); each change sends `companion.changed`; `companion.status` gains `memories`.
- Never log memory text at INFO level beyond the first 40 characters (the log ring is read over USB; memories are personal).
- Companion version 0.2.0; commit as papajorgioster@gmail.com; never commit `cartridges/companion/dev_key.h`.

## Review Focus

- The same thing remembered twice (the model forgets it already knows; or the app adds a copy) → no duplicate: identical text (case and spaces ignored) returns the existing id. (Test: Task 2 `dedupe`.)
- Memory text with quotes, newlines, accents or emoji, from the app or the model → stored and returned intact; the JSON file still loads. (Test: Task 2 `round trip with awkward text`.)
- `update_memory` / `forget_memory` with an id that no longer exists (deleted in the app since the session started) → ignored, nothing else changes. (Test: Task 2 `unknown id`.)
- A memory call in a turn nobody asked for, or in an interrupted reply's leftovers → not applied. (Test: Task 1 tracker tests.)
- A card file that's half-written or garbage → chats still work with no memories, the bad file is kept aside. (Test: Task 2 `broken file`; Task 4 device check.)

---

### Task 1: Tools in the protocol and the tracker (host)

**Files:**
- Modify: `cartridges/companion/live_protocol.h/.cpp`, `cartridges/companion/turn_tracker.h`
- Test: `tools/companion_test/test_live_protocol.cpp`, `tools/companion_test/test_turn_tracker.cpp`, new samples `tools/companion_test/live_samples/tool_calls_memory.json` (hand-made: one `toolCall` with `set_mood` + `remember` + `forget_memory`)

**Interfaces:**
- Produces: `struct ToolCall { std::string id, name, mood, text, group; int memoryId = -1; };` and `LiveEvent::calls` (`std::vector<ToolCall>`, replacing `toolCallId/toolName/mood`); `liveToolsOk(const std::vector<ToolCall> &answered)` → one `toolResponse` with a SILENT response per call (replaces `liveToolOk`); `systemInstruction(localTime, place, memorySection)` (third parameter: the store's section, "" = none; the tools' guidance is always included); `TurnTracker::message(bool content, const std::vector<std::string> &toolNames, bool turnComplete, int tokens)` with `Out::apply` (`std::vector<bool>`, per call: apply it) and `Out::answer` (per call: send its response) replacing `answerTool/showMood`.

- [ ] **Step 1: Write the failing tests.** In `test_live_protocol.cpp`: setup declares 4 functions named `set_mood, remember, update_memory, forget_memory`, each `NON_BLOCKING`; `remember.parameters.properties.group.enum` = the 5 groups; `update_memory` requires `id, text`; `forget_memory` requires `id`. Parse `tool_calls_memory.json` → 3 calls in order, `calls[1].text == "Your sister is called Ana"`, `calls[1].group == "people"`, `calls[2].memoryId == 12`. `liveToolsOk` of two calls → `functionResponses` has 2 entries with their ids and `scheduling: SILENT`. `systemInstruction("14:05", "", "")` contains "Never remember passwords" and has no "What you remember"; with a section it ends with that section. In `test_turn_tracker.cpp`: `set_mood` answered once per question (as now); a `remember` inside a question → `apply[i] == true`; any call when no question is open or while draining → `apply == false, answer == false`; the answer policy for memory calls is pinned in Task 3 (write it as "answered" now; Task 3 may flip it and its test).
- [ ] **Step 2: Run** `python3 tools/companion_test/run.py` → Expected: FAIL (compile errors on `calls`, `liveToolsOk`, the new `message` signature).
- [ ] **Step 3: Implement** the declarations (descriptions: remember = "Save something lasting about the user to remember in future chats", update_memory = "Change a memory (by its number) that is out of date", forget_memory = "Delete a memory (by its number) the user asked you to forget"), the parsing of every call, `liveToolsOk`, the instruction's guidance text (spec "Saving": what is worth remembering, what isn't, second person, short, update instead of repeating, never passwords/codes/secrets), and the tracker change. Update `gemini_live.cpp` to the new interfaces (set_mood → `cb_.mood`; memory calls → a new `Callbacks::memory(const ToolCall &)`, only when `apply`), keeping behaviour otherwise identical.
- [ ] **Step 4: Run** `python3 tools/companion_test/run.py` → Expected: all test files pass; `~/.platformio/penv/bin/pio run -e companion` → SUCCESS.
- [ ] **Step 5: Commit** "Companion: memory tools in the protocol".

### Task 2: MemoryStore (host)

**Files:**
- Create: `cartridges/companion/memory_store.h/.cpp` (plain C++ + ArduinoJson, no Arduino)
- Test: `tools/companion_test/test_memory_store.cpp` (`// uses: cartridges/companion/memory_store.cpp`)

**Interfaces:**
- Produces: `struct Memory { int id; std::string group, text, by; int64_t added, changed; };` `class MemoryStore { public: static constexpr int kMax = 300, kMaxText = 200; struct Added { bool ok; int id; std::string error; /* "full" | "empty" | "group" */ }; bool load(const std::string &json); std::string save() const; Added add(const std::string &text, const std::string &group, const std::string &by, int64_t now); bool update(int id, const std::string *text, const std::string *group, int64_t now); bool remove(int id); void clear(); const std::vector<Memory> &all() const; const Memory *find(int id) const; std::vector<Memory> page(int offset, int limit) const; /* newest first */ std::string promptSection() const; static std::string cut(const std::string &text); static bool validGroup(const std::string &g); static const char *label(const std::string &g); };`

- [ ] **Step 1: Write the failing tests:** `add` returns ids 1, 2, 3 and `save()`/`load()` round-trips them (including `next`, so a deleted id is never reused); `add` with group "bogus" → `error == "group"`; empty or all-spaces text → `"empty"`; the 301st add → `"full"`; `dedupe`: adding "you  love Swimming " after "You love swimming" → `ok`, the first id, count unchanged; `cut` of a 250-character sentence → ≤ 200 code points, ends at a word boundary, no broken UTF-8 (test with "é" and an emoji at the boundary); `round trip with awkward text`: `He said "hi"\nthen left — café 🎉` survives save/load; `unknown id`: `update(99, …)` and `remove(99)` → false, nothing changed; `update` changes text/group/`changed` but not `added`; `page(0, 2)` → the two newest, `page(2, 2)` → the next; `promptSection()` on an empty store → ""; with memories → the header line from the spec, groups in the fixed order with their labels, `[id] text` oldest first within a group, a group with no memories left out; when count == 300 the header adds "Memory is full (300): ask the user to delete some in the app before remembering more."; `broken file`: `load("{not json")` → false and the store is empty.
- [ ] **Step 2: Run** → Expected: FAIL (`memory_store.h` not found).
- [ ] **Step 3: Implement.** Dedupe key: lower-case ASCII letters, collapse whitespace, trim.
- [ ] **Step 4: Run** → Expected: all pass.
- [ ] **Step 5: Commit** "Companion: memory store".

### Task 3: Device check — how Gemini takes memory tool responses

**Files:**
- Modify: `cartridges/companion/main.cpp` (temporary log of every applied memory call; no store yet)

- [ ] **Step 1:** Flash with Task 1 (memory calls answered once each). Five chats that should trigger saves ("My sister is called Ana", "I hate mornings", "Remember my dentist is on the 3rd of November", "Actually my sister lives in Porto", "Forget my dentist appointment"), the session left open 60 s after each.
- [ ] **Step 2:** Count `turn complete … - not the reply` lines that follow no question and any `turns nobody asked for` closes. Expected: none. If any appear, flip the policy (memory calls applied but not answered), update Task 1's tracker test to pin it, repeat the five chats. Ledger the result either way: `Task 3: Ruling: memory tool responses <answered once | not answered> — <counts seen>`.
- [ ] **Step 3: Commit** the tracker policy (if changed) "Companion: memory tool response policy".

### Task 4: Memories on Dotty

**Files:**
- Create: `cartridges/companion/memories.h/.cpp` (card I/O around `MemoryStore`)
- Modify: `cartridges/companion/main.cpp`, `screens.h/.cpp`, `history.h/.cpp`, `usage_rules.h/.cpp` (history line gains `memory`)
- Test: `tools/companion_test/test_usage_rules.cpp` (history line with and without `memory`)

**Interfaces:**
- Consumes: `MemoryStore` (Task 2), `Callbacks::memory` (Task 1).
- Produces: `namespace memories { void begin(); /* loads the card file; broken → memories.bad.json */ MemoryStore &store(); bool save(); bool available(); }`; `historyLine(..., const std::string &memory = "")` (key `"memory"` only when non-empty); `history::append(..., const std::string &memory)`; `ScreenInfo::note` (String, "" = none).

- [ ] **Step 1: Write the failing test** in `test_usage_rules.cpp`: `historyLine(…, "remembered: Your sister is called Ana")` ends with `,"memory":"remembered: Your sister is called Ana"}`; without it the line is unchanged from today's expected string.
- [ ] **Step 2: Run** → FAIL (too many arguments). **Step 3: Implement** it, then `memories.*`, and in `main.cpp`: `memories::begin()` after `storage::begin()`; `connectLoop` passes `memories::store().promptSection()` (built on the main loop before the task starts, copied into the task's config); `cb.memory` applies remember/update/forget with `by = "dotty"`, saves, sets the note ("Remembered", "Updated", "Forgotten", or "Memory full" when add says full) shown for 3 s, collects "remembered: …" etc. for the turn's history line, logs at most the first 40 characters; `screens.cpp` draws the note with a 10×12 bookmark icon centred under the avatar when `info.note` is set (all states but Listening).
- [ ] **Step 4: Run** host tests → pass; build → SUCCESS. **Device check:** "My sister is called Ana" → "Remembered" on screen; reflash nothing, new chat (BOOT after the 3-min idle close or a lock/unlock) → "What's my sister's name?" → answer contains Ana; "Forget my sister's name" → "Forgotten"; replace the card file with "garbage" (dev key `M` writes it) → next boot logs the `.bad` rename and chats still work.
- [ ] **Step 5: Commit** "Companion: memories on Dotty".

### Task 5: BLE commands and the app's Memories screen

**Files:**
- Modify: `cartridges/companion/main.cpp` (`registerCommands`)
- Create: `ios/Dotty/Features/Companion/MemoriesView.swift`
- Modify: `ios/Dotty/Features/Companion/CompanionView.swift` (Memories row with the count)

**Interfaces:**
- Consumes: `memories::store()/save()` (Task 4).
- Produces: the spec's commands — `companion.memories {offset?, limit? = 50}` → `{memories, total, max: 300}` (newest first, `page`), `companion.memory.add {text, group}` → `{ok, id}` / `{ok: false, error}`, `companion.memory.update {id, text?, group?}`, `companion.memory.delete {id}`, `companion.memory.clear`; `companion.status.memories`.

- [ ] **Step 1: Implement the commands** (by = "app"; `cut` the text; every change saves and notifies; while `!memories::available()` every memory command replies `{ok: false, error: "unavailable"}` and the app shows "Memories unavailable" — the card is missing or unreadable).
- [ ] **Step 2: Implement MemoriesView:** loads every page (`offset` += 50 until `total`), search (case-insensitive substring), sections in the fixed group order with the spec's labels, "by Dotty"/"by you" + date caption, tap → edit sheet (TextEditor limited to 200 characters with a counter, group picker, Save / Delete), swipe to delete, "Add a memory", "Forget everything" with confirmation, errors via `NoticeCard` ("Memory full: delete some first" for `full`), reload on `companion.changed`, `.needsDotty(link)` on controls.
- [ ] **Step 3: Build:** `xcodebuild -project ios/Dotty.xcodeproj -scheme Dotty -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build` → BUILD SUCCEEDED; firmware build → SUCCESS. **Device/app check (the user's iPhone):** the memories from Task 4 are listed; add one in the app → next chat uses it; edit and delete work; search finds by any word.
- [ ] **Step 4: Commit** "Companion: memories in the app".

### Task 6: Ship

**Files:**
- Modify: `cartridges/companion/main.cpp` (`DOTTY_CARTRIDGE(..., "0.2.0")`, remove Task 3's temporary log), `CLAUDE.md` (Companion section: memories), `cartridge.json` description ("…and remembers what matters to you.")

- [ ] **Step 1:** Host tests pass, firmware and app build.
- [ ] **Step 2:** Commit, check no key in the outgoing commits (as in piece 1), push, `.venv/bin/python tools/build_catalog.py --release` → companion 0.2.0 listed.
