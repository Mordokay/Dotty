# Companion Scheduled Moments Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The companion speaks first at the right minute (reminders, alarms, timers, its own spontaneous lines), from lines written and recorded ahead, set by voice or in the app, with a general phone wake that reaches the iPhone while Dotty is locked.

**Architecture:** Plain C++ units (host-tested) hold every rule: the moments store and rules, the scheduler (due groups, late/missed, quiet windows, ringing), the timer's sound plan, the phone wake's stages, the line writer's requests and the recording job's steps. `moments.*` keeps them on the card; `main.cpp` glues them to the screen, the speaker, Gemini Live (a recording turn routes the reply audio to an `AdpcmRecorder` instead of the speaker) and the locked loop. The shell gains three small system features (the place, a wake at a given time, a Bluetooth window while locked). The app gets a Moments screen and sends its place.

**Tech Stack:** ESP32-S3 Arduino (pioarduino, the companion's custom core: mbedTLS in PSRAM), NimBLE-Arduino 2.x, ArduinoJson 7, FreeRTOS tasks; SwiftUI / CoreBluetooth / CoreLocation + MapKit (Swift 6).

**Spec:** `docs/superpowers/specs/2026-10-24-companion-moments-design.md`

## Global Constraints

- Kinds: reminder, alarm, timer, spontaneous. Styles: spoken, sung, whispered, excited, calm.
- At most 50 moments per companion; `what` ≤ 200 characters; ids never reused; one timer at a time, ≤ 24 h.
- Lines: written by the writer (`gemini-flash-lite-latest`), read word for word by Live, recorded; ≤ 150 words for one topic, + 100 per extra topic in a group, ≤ 300; read in ~100-word chunks (`readingChunks`).
- Attention beeps: 5 rising notes, ~2 s, then 0.5 s of quiet, then the line.
- Alarm: at `alarmVolume` (≥ 80 %, default 90 %), ignores quiet windows and mute, repeats every minute for up to 10 minutes; BOOT = done; tap = snooze 5 minutes, at most 3 (the 4th tap = BOOT); unanswered → "Missed alarm HH:MM" on the lock screen until the next unlock.
- Reminder / spontaneous: play once; holding BOOT within 3 minutes opens a chat that knows what was said.
- Late: a moment due < 1 hour ago fires; older is missed.
- Rules defaults: `perDay` 2 (0–5), allowed hours 10:00–21:00, no quiet windows, `quietMode` beeps, `alarmVolume` 90, `wakeEvery` 20 minutes.
- Spontaneous lines: ≥ 2 h apart, inside the allowed hours, outside quiet windows, ≥ 30 minutes from other moments, planned 30 minutes before the allowed hours start, the active companion only.
- Timer sounds: optional tick every `tickEvery` s (not in the last minute); ticks at 60 s and 30 s; the last 10 s a beep a second, a semitone higher each, volume 50 → 100 %; at 0 a fanfare, then the line; rings like an alarm (BOOT or tap stops, no snooze). Unlocked screen refresh every 10 s, every second in the last minute; locked bottom line once a minute; Dotty unlocks at 60 s left.
- Phone wake: every `wakeEvery` minutes while locked, only when a job is due; 30 s for the phone's `relay.hello`; jobs ≤ 3 minutes; no phone → Wi-Fi for jobs that can use it; Bluetooth off at the end.
- Sandbox: the companion reads only its own data and system data; `core.place {name, lat, lon}` → NVS `clock` (`place`, `lat`, `lon`, `placeAt`) → `shell::place()`.
- Times are local epoch seconds (Dotty's clock is local; TZ unset, `gmtime_r` gives local fields).
- Companion version 0.5.0. Never `Serial.print` (LOGI/W/E); keys only as their last 4 characters.
- Every cartridge change ends with commit → push → catalog release (`.venv/bin/python tools/build_catalog.py --release`, companion built last).

## Review Focus

- The clock moving under a moment (`core.time` or SNTP jumps minutes or hours): a moment never fires twice, and one jumped over fires (< 1 h) or becomes missed — never silently lost. Test: `test_moment_schedule` "clock jump".
- A light-sleep wake that comes early (the sleep clock drifts; no 32 kHz crystal): the moment still fires in its minute — the wake re-arms instead of skipping. Test: `test_moment_schedule` "early wake".
- A moment due while the user holds BOOT or a reply plays: it waits for the reply to end and never cuts speech. Test: `test_moment_schedule` "busy".
- A background recording interrupted (BOOT, lock, the phone leaving, quota): the job ends Failed, the moment stays not ready, no turn or history line leaks into the next chat. Test: `test_line_job` "abort".
- A phone wake that never sees the phone and has no Wi-Fi: Bluetooth goes off and Dotty sleeps again within 30 s — a wake can never keep Bluetooth on. Test: `test_phone_wake` "no phone, no wifi".

---

### Task 1: The place as system data, a wake at a given time, a Bluetooth window

**Files:**
- Modify: `lib/dotty_core/src/shell.h`, `lib/dotty_core/src/shell.cpp`
- Modify: `cartridges/companion/main.cpp` (`placeName()` → `shell::place`)
- Modify: `ios/Dotty/Bluetooth/DottyLink.swift` (`syncClock` also sends the place), `ios/Dotty/Features/Weather/WeatherView.swift` (`placeName(of:)` made `static func` internal, not private, so DottyLink can use it)
- Modify: `CLAUDE.md` (remove the "Known exception" sentence of the sandbox rule; document `core.place`, `wakeLockedAt`, `bluetoothWindow` in the shell's section)

**Interfaces:**
- Produces (C++):
  - `bool shell::place(String &name, double &lat, double &lon)` — false until a phone has sent one.
  - `void shell::wakeLockedAt(time_t local)` — the next locked sleep ends at that time if it's sooner than the next minute; `Config::onLockedWake` is then called (even with `lockedWakeSeconds` 0). One pending time; a later call replaces it; 0 clears it.
  - `void shell::bluetoothWindow(uint32_t seconds)`, `void shell::endBluetoothWindow()`, `bool shell::bluetoothWindowOpen()` — while locked: `ble::start()` + wake lock `power::kWakeLockBle` until the time is up or it's ended, then `ble::stop()` (if still locked) and the wake lock off. Unlocked: no effect (Bluetooth is on anyway).
  - BLE command `core.place {name, lat, lon}` (registered next to `core.time`; `name` ≤ 60 bytes; replies `{ok}`).
- Produces (Swift): `DottyLink.syncClock()` sends `core.place` after `core.time` when `CLLocationManager().authorizationStatus` is `.authorizedWhenInUse` or `.authorizedAlways` (never prompts), using `OneShotLocation().current()` and `WeatherView.placeName(of:)`; at most once per 20 minutes (a stored `lastPlaceAt`).

- [ ] **Step 1:** In `shell.cpp`, keep `wakeAtMs` (millis when the wake is due, computed from `local - time(nullptr)` at the call); the locked sleep computation takes `min(sleepMs, wakeAtMs - millis())` (≥ 100 ms); `runLockedWake()` also runs when `wakeAtMs` has passed, then clears it. The companion re-arms from its hook (an early wake simply re-arms).
- [ ] **Step 2:** `bluetoothWindow`: a deadline checked in `update()` (locked branch and unlocked); the locked branch's `canSleep` is already false while a wake lock is held, so the loop keeps running and `ble::poll()` serves the phone. Log `bluetooth window: N s` and `bluetooth window closed after N s`.
- [ ] **Step 3:** `core.place` stores `place`, `lat`, `lon`, `placeAt` (local epoch) in NVS `clock`; `shell::place` caches the read like `utcOffset`.
- [ ] **Step 4:** Companion: delete `placeName()`; `sessionConfig()` uses `shell::place(name, lat, lon) ? name : ""`. No `weather` namespace read remains: `grep -n '"weather"' cartridges/companion/*.cpp` → nothing.
- [ ] **Step 5:** Build: `~/.platformio/penv/bin/pio run -e launcher && ~/.platformio/penv/bin/pio run -e companion` → SUCCESS both (launcher first: the companion's custom core is rebuilt last). `xcodebuild -project ios/Dotty.xcodeproj -scheme Dotty -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build` → BUILD SUCCEEDED.
- [ ] **Step 6:** Commit: `Shell: core.place (system data), wakeLockedAt, a Bluetooth window while locked; the companion stops reading Weather's settings`.

### Task 2: Moments store and rules (host-tested)

**Files:**
- Create: `cartridges/companion/moment_store.h/.cpp`, `tools/companion_test/test_moment_store.cpp` (`// uses: cartridges/companion/moment_store.cpp`)

**Interfaces:**
- Produces:
  ```cpp
  enum class MomentKind { Reminder, Alarm, Spontaneous };  // the timer is kept apart (timer_beeps.h)
  enum class Repeat { Once, Daily, Weekdays, Days };
  enum class MomentState { Upcoming, Done, Missed };  // ringing / snoozed are runtime only (Ringing)
  struct Moment {
    int id = 0; MomentKind kind = MomentKind::Reminder; int64_t at = 0;  // the next time it's due
    Repeat repeat = Repeat::Once; uint8_t days = 0;  // bit 0 = Monday … bit 6 = Sunday (Repeat::Days)
    std::string what, style = "spoken", source = "chat", line;  // line: the text once written
    bool ready = false; MomentState state = MomentState::Upcoming; int64_t created = 0;
  };
  class MomentList {
   public:
    static constexpr int kMax = 50, kWhatMax = 200;
    bool load(const std::string &json);   // {"next": N, "moments": [...]}; false = broken
    std::string save() const;
    const std::vector<Moment> &all() const;
    const Moment *find(int id) const; Moment *find(int id);
    int add(Moment m);                    // -1: full, empty what, at ≤ 0, or a bad style
    bool update(const Moment &m);         // by id; what/at/style/kind/repeat/days; ready=false when what or style changed
    bool remove(int id);
  };
  struct QuietWindow { uint8_t days = 0x7F; int from = 0, to = 0; };  // minutes since midnight; to < from = past midnight
  struct MomentRules {
    int perDay = 2, from = 600, to = 1260; std::vector<QuietWindow> quiet; bool quietSilent = false;
    int alarmVolume = 90, wakeEvery = 20;
    bool load(const std::string &json); std::string save() const;  // keys: perDay, from, to, quiet[{days,from,to}], quietMode ("beeps"|"silent"), alarmVolume, wakeEvery
    bool inQuiet(int64_t local) const; bool inAllowedHours(int64_t local) const;
  };
  bool parseLocalTime(const std::string &iso, int64_t &local);   // "YYYY-MM-DDTHH:MM" (seconds optional)
  std::string localTimeText(int64_t local);                      // "2026-10-24T18:00"
  int64_t nextOccurrence(const Moment &m, int64_t after);        // first due time > after; Once: m.at if > after, else 0
  const char *kindName(MomentKind); bool kindFromName(const std::string &, MomentKind &);
  bool validStyle(const std::string &);
  ```

- [ ] **Step 1: Write the failing test** `test_moment_store.cpp` with these checks (names as given):
  - "add: ids grow, never reused" — add 3, remove id 2, add → id 4.
  - "add: 50 max" — the 51st returns -1.
  - "add: refuses empty what, bad style, at 0" — each -1.
  - "what cut to 200 characters at a word" — a 260-char text stored ≤ 200 and ending on a whole word.
  - "save/load round trip" — every field of two moments (one `Days` with `days = 0b0010101`) equal after `load(save())`.
  - "load: broken json is false" — `load("{x")` false, list empty.
  - "update: changed what clears ready" — ready true → update with new what → false; update of only `at` keeps ready.
  - "parseLocalTime" — `"2026-10-24T18:00"` → 1793124000 (local epoch, computed with `timegm` in the test); `"2026-13-01T10:00"`, `"6pm"` → false.
  - "nextOccurrence: daily" — at 07:30 Friday 2026-10-23, after Saturday 08:00 → Sunday 07:30.
  - "nextOccurrence: weekdays skip the weekend" — after Friday 07:31 → Monday 07:30.
  - "nextOccurrence: chosen days" — Mon+Wed, after Monday 09:00 (at 08:00) → Wednesday 08:00.
  - "nextOccurrence: once in the past is 0".
  - "rules: defaults" — perDay 2, from 600, to 1260, alarmVolume 90, wakeEvery 20, quietSilent false.
  - "rules: clamps" — perDay 9 → 5, alarmVolume 50 → 80, wakeEvery 1 → 10, wakeEvery 999 → 240.
  - "rules: quiet window past midnight" — {all days, 23:00–07:00}: 02:00 quiet, 12:00 not.
  - "rules: quiet window on weekdays only" — {Mon–Fri, 09:00–17:00}: Monday 10:00 quiet, Saturday 10:00 not.
- [ ] **Step 2:** Run `python3 tools/companion_test/run.py moment_store` → BUILD FAILED (no `moment_store.h`).
- [ ] **Step 3:** Implement `moment_store.h/.cpp` (ArduinoJson; days of week from `gmtime_r` on the local epoch; the 200-character cut at a word like `memory_store`'s).
- [ ] **Step 4:** Run it → all PASS; then `python3 tools/companion_test/run.py` → every file passes.
- [ ] **Step 5:** Commit: `Companion: moments store and rules (host-tested)`.

### Task 3: The scheduler: due groups, late and missed, quiet windows, ringing (host-tested)

**Files:**
- Create: `cartridges/companion/moment_schedule.h/.cpp`, `tools/companion_test/test_moment_schedule.cpp` (`// uses: cartridges/companion/moment_schedule.cpp cartridges/companion/moment_store.cpp`)

**Interfaces:**
- Consumes: Task 2's `Moment`, `MomentList`, `MomentRules`, `nextOccurrence`.
- Produces:
  ```cpp
  struct CompanionMoments { std::string companion; MomentList *list; };  // in companion order
  enum class Fate { Wait, Fire, Missed };
  Fate fate(int64_t at, int64_t now);           // at > now: Wait; now - at < 3600: Fire; else Missed
  struct FireGroup { std::string companion; int64_t minute; std::vector<int> ids; bool alarm; };
  struct Due { std::vector<FireGroup> fire; std::vector<std::pair<std::string, int>> missed; };
  // Upcoming moments whose time has come, grouped per companion and minute (at / 60); the busy flag
  // (listening or a reply playing) returns nothing to fire yet (missed ones are still reported).
  Due due(const std::vector<CompanionMoments> &all, int64_t now, bool busy);
  void advance(Moment &m, int64_t now, bool missed);  // repeating: at = nextOccurrence(m, now), Upcoming; once: Done / Missed
  int64_t nextDue(const std::vector<CompanionMoments> &all);  // earliest Upcoming at, 0 = none
  enum class Delivery { Voice, BeepsAndText, TextOnly };
  Delivery delivery(const MomentRules &rules, bool alarm, int64_t now);  // alarms: Voice; quiet: BeepsAndText or TextOnly
  int groupWords(size_t topics);  // 150 + 100 × (topics − 1), at most 300
  // The signature a group's recording was made for (member ids, what and style): a change means record again.
  std::string groupSignature(const std::vector<const Moment *> &members);
  // Upcoming one-off moments of one companion sharing a minute with another (they get one line together).
  std::vector<std::vector<int>> sharedMinutes(const MomentList &list);
  class Ringing {  // an alarm (or a timer's end) ringing
   public:
    static constexpr uint32_t kRepeatMs = 60000, kMaxMs = 600000, kSnoozeMs = 300000;
    static constexpr int kMaxSnoozes = 3;
    enum class Action { None, Play, GiveUp };
    void start(uint32_t now, bool snoozable);
    Action poll(uint32_t now);   // Play at start and every minute while ringing; GiveUp after 10 minutes unanswered
    bool snooze(uint32_t now);   // false (and stopped) when not snoozable or the 4th time
    void stop();
    bool active() const; bool snoozed() const; int snoozes() const;
  };
  ```

- [ ] **Step 1: Write the failing test** `test_moment_schedule.cpp`:
  - "fate: 59 minutes late fires, 61 is missed, future waits".
  - "due: two of a companion at one minute are one group" — ids {1, 2} in one `FireGroup`.
  - "due: other companions apart, in companion order" — friend + coach at 18:00 → two groups, friend's first when friend is first in `all`.
  - "due: the alarm makes the group an alarm" — reminder + alarm → `alarm` true.
  - "busy: nothing fires while busy" — `due(…, busy=true).fire` empty; same call with busy false fires it.
  - "clock jump: forward 30 minutes past a moment fires it once" — due at 18:00, now 18:30 → fires; after `advance`, `due` at 18:31 fires nothing.
  - "clock jump: forward 2 hours makes it missed".
  - "clock jump: backwards doesn't fire it again" — a fired daily moment (advanced to tomorrow) with now moved back 10 minutes → nothing due.
  - "early wake: a wake 2 s before the minute doesn't skip it" — now = 17:59:58 → Wait; 18:00:01 → Fire.
  - "advance: repeating moves on, one-off is done".
  - "delivery: quiet reminder beeps or text only; alarm always voice".
  - "groupWords: 150, 250, 300, 300 for 1–4 topics".
  - "groupSignature changes with what or style, not with at".
  - "sharedMinutes: two one-offs at one minute, a daily one isn't counted".
  - "ringing: plays at 0, 60, 120 s; gives up at 600 s".
  - "ringing: three snoozes, the fourth stops" — snooze returns true ×3, false the 4th, `active()` false.
  - "ringing: a snooze rings again 5 minutes later and restarts the 10 minutes".
  - "ringing: not snoozable (timer) → snooze stops it".
- [ ] **Step 2:** Run `python3 tools/companion_test/run.py moment_schedule` → BUILD FAILED.
- [ ] **Step 3:** Implement `moment_schedule.h/.cpp`.
- [ ] **Step 4:** Run it → PASS; whole suite → every file passes.
- [ ] **Step 5:** Commit: `Companion: moments scheduler — groups, late/missed, quiet windows, ringing (host-tested)`.

### Task 4: The timer and its sounds (host-tested)

**Files:**
- Create: `cartridges/companion/timer_beeps.h/.cpp`, `tools/companion_test/test_timer_beeps.cpp` (`// uses: cartridges/companion/timer_beeps.cpp`)

**Interfaces:**
- Produces:
  ```cpp
  struct TimerState {
    bool running = false, paused = false; int64_t endsAt = 0; int left = 0;  // left: seconds, while paused
    int tickEvery = 0; std::string label, companion, line; bool ready = false;
    static constexpr int kMaxSeconds = 86400;
    bool start(int seconds, const std::string &label, int tickEvery, const std::string &companion, int64_t now);  // false: ≤ 0 or > 24 h
    void pause(int64_t now); void resume(int64_t now); void cancel();
    int secondsLeft(int64_t now) const;   // never below 0
    std::string lockLine(int64_t now) const;  // "Pasta · 7 min" (minutes rounded up; "Timer" without a label)
    std::string clock(int64_t now) const;     // "9:41", "1:02:05"
  };
  struct Beep { uint8_t midi; uint16_t ms; uint8_t volume; };
  enum class TimerSound { None, Tick, Countdown, End };
  struct TimerCue { TimerSound sound = TimerSound::None; Beep beep{}; };
  TimerCue timerCue(int secondsLeft, int tickEvery, bool busy);  // asked once for each second reached
  int timerRefreshSeconds(int secondsLeft);  // 10, or 1 in the last minute
  ```
  Values: Tick = midi 84, 30 ms, volume 40; Countdown at s = 10…1: midi 72 + (10 − s), 120 ms, volume 50 + (10 − s) × 50 / 9 (50 … 100); End at 0 (the fanfare is played by main.cpp).

- [ ] **Step 1: Write the failing test** `test_timer_beeps.cpp`:
  - "start: 0 and 24 h + 1 refused; 10 minutes runs".
  - "secondsLeft counts down; pause holds; resume carries on" — start 600 at t, pause at t+100 (500 left), resume at t+200 → 500 left, endsAt = t+700.
  - "lockLine: Pasta · 7 min; Timer · 1 min at 30 s".
  - "clock: 9:41, 1:02:05, 0:00".
  - "cue: no tickEvery → silence at 300 s".
  - "cue: tickEvery 60 → a tick at 120 s, none at 90".
  - "cue: ticks at 60 and 30 even without tickEvery".
  - "cue: no interval tick in the last minute" — tickEvery 10 at 50 s → None.
  - "cue: countdown 10 → 1 rises a semitone, volume 50 → 100" — midi 72 at 10, 81 at 1; volume 50 at 10, 100 at 1.
  - "cue: 0 is the end".
  - "cue: busy skips ticks and countdown, never the end".
  - "refresh every 10 s, every second in the last minute".
- [ ] **Step 2:** Run `python3 tools/companion_test/run.py timer_beeps` → BUILD FAILED.
- [ ] **Step 3:** Implement `timer_beeps.h/.cpp`.
- [ ] **Step 4:** Run it → PASS; whole suite → every file passes.
- [ ] **Step 5:** Commit: `Companion: the timer and its beep plan (host-tested)`.

### Task 5: The phone wake (host-tested)

**Files:**
- Create: `cartridges/companion/phone_wake.h`, `tools/companion_test/test_phone_wake.cpp`

**Interfaces:**
- Produces:
  ```cpp
  class PhoneWake {
   public:
    static constexpr uint32_t kPhoneWaitMs = 30000, kJobsMaxMs = 180000;
    enum class Stage { Idle, WaitingForPhone, Phone, Wifi, Done };
    // The next wake's time (local epoch): lastWake + wakeEvery minutes when a job is due, else 0.
    int64_t nextWakeAt(int64_t lastWake, int wakeEveryMinutes, bool jobDue) const;
    void start(uint32_t nowMs);
    // phoneReady: relay.hello said and a key in common; wifiJobs: a due job can use Wi-Fi; jobsDone: nothing left.
    Stage poll(uint32_t nowMs, bool phoneReady, bool wifiJobs, bool jobsDone);
    bool bluetoothOn() const;   // WaitingForPhone or Phone
    Stage stage() const;
    void end();                 // → Idle (a chat, BOOT or a firing moment cancels a wake)
  };
  ```

- [ ] **Step 1: Write the failing test** `test_phone_wake.cpp`:
  - "nextWakeAt: nothing due → 0; due → last + 20 minutes".
  - "phone in 5 s → Phone, Bluetooth on".
  - "no phone in 30 s, Wi-Fi jobs → Wifi, Bluetooth off".
  - "no phone, no wifi: Done at 30 s, Bluetooth off" (Review Focus).
  - "jobs done → Done at once".
  - "jobs over 3 minutes → Done" — Phone at 5 s, still not done at 185 s → Done.
  - "end() → Idle, Bluetooth off".
- [ ] **Step 2:** Run `python3 tools/companion_test/run.py phone_wake` → BUILD FAILED.
- [ ] **Step 3:** Implement `phone_wake.h` (header-only, like `relay_path.h`).
- [ ] **Step 4:** Run it → PASS; whole suite → every file passes.
- [ ] **Step 5:** Commit: `Companion: the phone wake's stages (host-tested)`.

### Task 6: Writing lines and plans; the recording job (host-tested)

**Files:**
- Create: `cartridges/companion/moment_lines.h/.cpp`, `cartridges/companion/line_job.h`, `tools/companion_test/test_moment_lines.cpp` (`// uses: cartridges/companion/moment_lines.cpp cartridges/companion/moment_store.cpp cartridges/companion/moment_schedule.cpp cartridges/companion/script_writer.cpp cartridges/companion/companion_store.cpp cartridges/companion/persona.cpp`), `tools/companion_test/test_line_job.cpp` (same uses)

**Interfaces:**
- Consumes: `Profile`, `personaText` (persona.h), `Written`, `parseWritten`, `readingChunks`, `continueReadingNote` (script_writer.h), Task 2/3 types.
- Produces (`moment_lines.h`):
  ```cpp
  struct Topic { std::string kind, what, style; };
  std::string styleNote(const std::string &style);  // sung → "sing it to a simple, cheerful melody"; whispered / excited / calm → that delivery; spoken → ""
  // generateContent body: the companion's character + memories, the topics, when it will play, ≤ words, "as short as the message allows".
  std::string lineRequest(const Profile &p, const std::string &memories, const std::vector<Topic> &topics, int64_t at, int words);
  std::string readLineNote(const std::string &text, const std::string &style);  // read aloud exactly, word for word, {styleNote}
  std::string recordingInstruction(const std::string &persona);  // a recording session's system instruction
  std::string planRequest(const Profile &p, const std::string &memories, const std::string &summaries, int64_t day,
                          const MomentRules &rules, int count, const std::vector<int64_t> &taken);
  struct PlannedLine { int64_t at; std::string what, line, style; };
  // The plan's JSON ({"lines": [{at: "HH:MM", what, line, style}]}) checked: allowed hours, not quiet, ≥ 2 h apart,
  // ≥ 30 min from `taken`, ≤ count, ≤ 150 words, a valid style (else "spoken"); the others dropped.
  std::vector<PlannedLine> parsePlan(int httpStatus, const std::string &body, int64_t day, const MomentRules &rules,
                                     const std::vector<int64_t> &taken, int count);
  bool planDue(int64_t now, int64_t lastPlannedDay, const MomentRules &rules);  // once a day, from (from − 30 min)
  std::string momentsSection(const MomentList &list, int64_t now);  // instruction: "You have N moments set; next: …" (3 at most), "" for none
  std::string listMomentsText(const MomentList &list, int64_t now); // list_moments' answer: every upcoming one, "id 3: Today 18:00, reminder: cook dinner (sung)"
  std::string firedNote(const std::string &line, const std::string &kind);  // "You just reminded them: …" for the chat that follows
  ```
- Produces (`line_job.h`, header-only):
  ```cpp
  struct LineWork {
    std::string companion, key;   // key: "<id>", "group-<minute>" or "timer"
    std::vector<int> ids; std::vector<Topic> topics; int64_t at = 0; int words = 150; std::string style, signature;
  };
  class LineJob {
   public:
    enum class Step { Idle, Writing, WaitSession, Reading, Saving, Done, Failed };
    void start(const LineWork &w);            // → Writing
    void written(const Written &w);           // ok → WaitSession (chunks of 100 words), else Failed
    bool takeSend(std::string &note);         // in WaitSession once sessionReady(), or after a chunk: the next note (readLineNote, then continueReadingNote)
    void sessionReady();                      // WaitSession → Reading
    void chunkDone();                         // a reading turn ended: more → takeSend gives it; none → Saving
    void saved(bool ok);                      // → Done / Failed
    void abort();                             // → Failed from any step (BOOT, lock, phone lost, quota)
    Step step() const; const LineWork &work() const; const std::string &text() const;
  };
  ```

- [ ] **Step 1: Write the failing tests**:
  - `test_moment_lines.cpp`: "styleNote: sung asks for a melody, spoken is empty"; "lineRequest: has every topic's what, the word limit and the companion's name"; "lineRequest: two topics ask for one line covering both, most pressing first"; "readLineNote: word for word, with the style"; "parsePlan: keeps two good lines"; "parsePlan: drops one outside the allowed hours, one in a quiet window, one < 2 h after another, one < 30 min from a taken time"; "parsePlan: over count keeps the first count"; "parsePlan: a bad style becomes spoken"; "parsePlan: HTTP 500 → none"; "planDue: 09:30 yes once, 09:29 no, again the next day"; "momentsSection: empty list → empty"; "momentsSection: 5 moments → count 5 and the next 3"; "listMomentsText: ids, times and kinds"; "firedNote: reminder".
  - `test_line_job.cpp`: "happy path: Writing → WaitSession → Reading → Saving → Done" (a 250-word text → 3 notes taken, the first `readLineNote`, the others `continueReadingNote`); "written failure → Failed"; "no send before sessionReady"; "abort from Reading → Failed, takeSend gives nothing" (Review Focus); "saved(false) → Failed".
- [ ] **Step 2:** Run `python3 tools/companion_test/run.py moment_lines line_job` → BUILD FAILED.
- [ ] **Step 3:** Implement `moment_lines.h/.cpp` and `line_job.h`. The plan request asks for JSON only (`responseMimeType: application/json` in `generationConfig`), the kinds of line from the spec (a callback to something said, a question about plans, a joke, "I miss you"), and the times as "HH:MM" on `day`.
- [ ] **Step 4:** Run them → PASS; whole suite → every file passes.
- [ ] **Step 5:** Commit: `Companion: moment lines, the daily plan and the recording job (host-tested)`.

### Task 7: The moment tools in the Live protocol (host-tested)

**Files:**
- Modify: `cartridges/companion/live_protocol.h/.cpp`, `cartridges/companion/gemini_live.h/.cpp`, `cartridges/companion/turn_tracker.h`
- Test: `tools/companion_test/test_live_protocol.cpp`, `tools/companion_test/test_turn_tracker.cpp`

**Interfaces:**
- Produces:
  - `ToolCall` gains `std::string kind, at, repeat, style, label; std::vector<std::string> days; int tickEvery = 0; std::string result;` (`what` keeps travelling as `text`, `id` as `memoryId`, `seconds` as `seconds`).
  - Declarations (NON_BLOCKING): `set_moment {kind: reminder|alarm, at: "YYYY-MM-DDTHH:MM", what, style?, repeat?: once|daily|weekdays|days, days?: [mon…sun]}`, `cancel_moment {id}`, `list_moments {}`, `start_timer {seconds, label?, tickEvery?}`, `stop_timer {}`.
  - `liveToolsOk` puts a non-empty `call.result` in `response["result"]` instead of "ok".
  - `GeminiLive::Callbacks` gains `std::function<ToolAnswer(const ToolCall &)> tool;` with `struct ToolAnswer { std::string result; bool speak = false; };` — called for the five moment tools; `speak` false → the result goes in the SILENT `liveToolsOk`; true → `answerTool(id, name, result)` after it (WHEN_IDLE: the model talks about it — an error to correct, or the list).
  - `TurnTracker`: `list_moments` is applied but not auto-answered (like `recall`; answered through `answerTool`).
  - Instruction rules (in `systemInstruction`'s fixed text): set moments only when asked; turn "at 6 pm" / "in 20 minutes" into a time from the date and time given; confirm briefly in your own words; a timer over 24 h isn't possible.

- [ ] **Step 1: Write the failing tests**: in `test_live_protocol.cpp` — "setup declares set_moment, cancel_moment, list_moments, start_timer, stop_timer"; "parse set_moment args" (a captured-shape toolCall JSON with kind alarm, at "2026-10-25T07:30", what, style excited, repeat days, days ["mon","wed"] → every field); "parse start_timer: seconds 600, label pasta, tickEvery 60"; "toolsOk carries a result" (`result = "{\"id\":4}"` → response.result is that text). In `test_turn_tracker.cpp` — "list_moments applied, not answered".
- [ ] **Step 2:** Run `python3 tools/companion_test/run.py live_protocol turn_tracker` → FAIL on the new checks.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run → PASS; whole suite → every file passes; `pio run -e companion` → SUCCESS (the `tool` callback unset is fine for now: unset = `{ "ok" }`).
- [ ] **Step 5:** Commit: `Companion: set_moment, cancel_moment, list_moments, start_timer, stop_timer in the Live protocol`.

### Task 8: Moments on the card

**Files:**
- Create: `cartridges/companion/moments.h/.cpp`
- Modify: `cartridges/companion/main.cpp` (`setup()` calls `moments::begin()` after `companions::begin()`)

**Interfaces:**
- Consumes: Tasks 2–4, `companions::folder`, `companions::list().order()`.
- Produces (`namespace moments`):
  ```cpp
  void begin();                         // loads every companion's moments.json, the rules, the timer (NVS); deletes recordings with no moment
  bool available();                     // the card is there
  std::vector<CompanionMoments> all();  // in companion order
  MomentList *of(const std::string &companion);   // nullptr: unknown companion
  bool save(const std::string &companion);        // moments.json via .part; a broken file at load → moments.bad.json
  MomentRules &rules(); bool saveRules();         // data/moment_rules.json
  TimerState &timer(); void saveTimer();          // NVS companion: timerOn, timerEnd, timerLeft, timerPaused, timerEvery, timerLabel, timerWho, timerReady
  String recordingPath(const std::string &companion, const std::string &key, const char *ext);  // <folder>/moments/<key>.wav|json
  bool recorded(const std::string &companion, const std::string &key, const std::string &signature);  // the WAV exists and its .json's "signature" matches
  void dropRecording(const std::string &companion, const std::string &key);
  void companionRemoved(const std::string &companion);  // forget its list (its folder goes with it)
  ```

- [ ] **Step 1:** Implement `moments.*` (the stores' JSON through the card like `macros.json`; the recording's `.json` = `lastLineJson` plus `"signature"`). Boot cleanup: a `moments/*.wav` whose key is no moment's id, no `group-<minute>` of a current shared minute, and not "timer" is deleted (logged with its name).
- [ ] **Step 2:** Build `pio run -e companion` → SUCCESS.
- [ ] **Step 3:** Device: flash (`pio run -e companion -t upload --upload-port /dev/cu.usbmodem1101`), log over serial (`d`) shows `moments: N companions, M moments, rules ok` at boot; a fake `moments.json` with garbage in Debug's folder → `moments.bad.json` and an empty list, no crash.
- [ ] **Step 4:** Commit: `Companion: moments, rules and the timer kept on the card`.

### Task 9: Setting moments by voice

**Files:**
- Modify: `cartridges/companion/main.cpp` (the `tool` callback; `connect()` adds `momentsSection`)

**Interfaces:**
- Consumes: Task 7's `ToolAnswer`, Task 8's `moments::*`, Task 6's `momentsSection`, `listMomentsText`.
- Produces: `ToolAnswer applyMomentTool(const ToolCall &c)` in main.cpp; `bool inChatLineWanted` + `LineWork pendingChatLine` (consumed by Task 11).

- [ ] **Step 1:** `applyMomentTool`:
  - `set_moment`: `parseLocalTime(c.at)`; in the past (≤ now) → `{error: "in the past"}`, speak; full → `{error: "full"}`, speak; else add (source "chat", the session's companion), save, `{id, at}`, silent; queue its line for this chat (Task 11) and show a bookmark note "Saving your reminder..." (alarm: "Saving your alarm...").
  - `cancel_moment` → remove + drop its recording → `{ok}` or `{error: "no such moment"}` (speak).
  - `list_moments` → `listMomentsText`, speak.
  - `start_timer`: a timer running → `{error: "a timer is running", label, left}`, speak; else `TimerState::start`, `saveTimer()`, `{endsAt}` silent, queue the timer's line ("timer" key).
  - `stop_timer` → cancel + `{ok}`.
  - Any of them on a companion other than the session's (switched mid-chat), or the moment ones without a card
    (`!moments::available()`) → `{error: "unavailable"}`, speak (timers still work without a card).
  - Every change: `notifyMoments()` (Task 13 defines the event; until then a stub that logs).
- [ ] **Step 2:** `connect()`: `personaSection += "\n\n" + momentsSection(…)` when not empty.
- [ ] **Step 3:** Build → SUCCESS. Device (Wi-Fi chat): "remind me in 5 minutes to stretch" → log `set_moment: id 1 at 2026-…`, the companion confirms; "what moments do I have?" → it lists it; "cancel it" → removed; "set a timer for 2 minutes for tea" → timer saved; "start another timer" → it asks first. Read with `d`.
- [ ] **Step 4:** Commit: `Companion: moments and timers set, listed and cancelled by voice`.

### Task 10: Moments fire

**Files:**
- Modify: `cartridges/companion/main.cpp`, `cartridges/companion/screens.h/.cpp`

**Interfaces:**
- Consumes: Task 3's `due`, `advance`, `nextDue`, `delivery`, `Ringing`; Task 8.
- Produces: `screens::drawMoment(const MomentView &v)` with `struct MomentView { String avatar, mood, kindIcon /* "bell"|"alarm"|"timer"|"speech" */, text; int page = 0; bool snoozable = false; };`; in main.cpp `void pollMoments()`, `bool momentBusy()` (listening, a reply in the speaker, a session reply due), `void fireGroup(const FireGroup &g)`, `uint32_t momentReplyUntil` (the 3-minute "reply" window), `String missedAlarmLine`.

- [ ] **Step 1:** `pollMoments()` (every loop pass, unlocked and locked): `due(moments::all(), now, momentBusy())`; missed → `advance(…, true)`, save, history line `{you: "", dotty: "(missed) " + what, moment: kind}`, an alarm sets `missedAlarmLine = "Missed alarm HH:MM"`; fire → `shell::wake()` (unlock), `fireGroup`.
- [ ] **Step 2:** Several groups at once (different companions): fired one after the other in `due`'s order, the attention beeps only before the first.
- [ ] **Step 2b:** `fireGroup`: the moment screen (avatar, kind icon, the group's line or the members' `what`); `Delivery`: Voice → the attention beeps (`playNotes`: 5 notes rising C5 E5 G5 C6 E6, 300 ms each with 100 ms gaps ≈ 2 s), 500 ms, then `player.play(recordingPath(…, "wav"))` with the text paced from the recording's `.json` like the replay (`LastLine::textAt`); no recording → members' recordings in a row (0.7 s apart), then the text of the rest; BeepsAndText → beeps + text; TextOnly → text. Alarms at `rules().alarmVolume`, else the companion's volume. Then `advance` each member, save, a history line per group `{you: "", dotty: line, moment: kind}`, `notifyMoments()`.
- [ ] **Step 3:** Alarm groups: `Ringing::start(now, true)`; `Play` → beeps + line again; BOOT → stop, "Good morning!" style one-line note from the group (no chat); tap → `snooze` (note "Snoozed until 07:35"); `GiveUp` → `missedAlarmLine`. Reminder / spontaneous: `momentReplyUntil = millis() + 180000`; a BOOT hold in that window starts a chat whose instruction ends with `firedNote(line, kind)`.
- [ ] **Step 4:** The locked side: `config.onLockedWake = pollMomentsLocked` (runs `pollMoments`, returns true when the lock line changed) and after every pass `shell::wakeLockedAt(next)` where next = the earliest of `nextDue(…)`, the timer's next cue (Task 12) and the next phone wake (Task 14); `config.nowPlaying` returns `missedAlarmLine` (cleared on unlock), else the timer's `lockLine` (Task 12), else "".
- [ ] **Step 5:** Boot: `pollMoments()` once after `moments::begin()` (the < 1 h late rule).
- [ ] **Step 6:** Build → SUCCESS. Device: a reminder (set by voice, Task 9) two minutes out, then lock → at its minute Dotty unlocks, beeps, shows the text (no recording yet: beeps + text). An alarm one minute out: rings each minute, three taps snooze, the fourth stops; an unanswered one gives up after 10 minutes → "Missed alarm HH:MM" on the lock screen. A reminder at the same minute as an alarm → one screen, rings as an alarm. Switch to another cartridge across a moment, come back 10 minutes later → it fires; 70 minutes later → missed.
- [ ] **Step 7:** Commit: `Companion: moments fire — beeps, the line, alarms with snooze, missed ones`.

### Task 11: Recording the lines

**Files:**
- Modify: `cartridges/companion/main.cpp`

**Interfaces:**
- Consumes: Task 6's `LineJob`, `lineRequest`, `readLineNote`, `recordingInstruction`, `groupWords`; Task 3's `sharedMinutes`, `groupSignature`; Task 8's `recorded`, `recordingPath`; the existing `jobs` (`JobSlot<Written>`), `launchJob`, `relayFetch`, `AdpcmRecorder`, `lastSaveTask` pattern, `sessionConfig()`.
- Produces: `LineJob lineJob`; `bool nextLineWork(LineWork &out)` (the earliest-due unready item: a timer line, a shared minute's group, else a single moment's own line; `ready` updated in the lists); `bool capturingLine` (cb.audio / cb.said route to the line's recorder while true); `void pollLines(bool inChat)`; `LiveConfig recordingConfig(const Profile &p)` (that companion's voice, `recordingInstruction(personaText(p))`, no tools but none needed); `enum class Job { Write, Recall, Line, Plan }`.

- [ ] **Step 1:** `pollLines`: when `lineJob` is Idle and nothing else uses `jobs` (`!writing && !reading && !jobs.busy()`) and `nextLineWork` gives work → `lineJob.start`, `writerBody = lineRequest(…)`, `jobKind = Job::Line`, `launchJob()` (relay when connected through the phone, else HTTPS). `jobs.take` for a `Job::Line` → `lineJob.written`.
- [ ] **Step 2:** In a chat: only after the current reply has ended (`info.state` After or Ready, `!writing && !reading`); `lineJob.sessionReady()` on the chat's session when its companion is the work's; `takeSend` → `live.sendText(note)` with `capturingLine = true`; cb.turnDone during capture → `lineJob.chunkDone()` (no history line, no `usage::addTurn`, tokens via `usage::addTokens`); then the recorder is saved by a task like `lastSaveTask` into `recordingPath(…)` with the signature → `lineJob.saved`. BOOT pressed during capture → `lineJob.abort()`, `capturingLine = false`, the session's tracker drains it (`TurnTracker::start` handles leftovers). The end-of-chat review waits until `lineJob` is Idle/Done/Failed (≤ 60 s), so lines asked in the chat are recorded before the session closes.
- [ ] **Step 3:** The screen: while a line records in a chat, the note (bookmark + "Saving your reminder...") stays up; nothing plays from the speaker.
- [ ] **Step 4:** `ready` set on every member and `notifyMoments()` after a save; a failed job leaves it not ready (retried later: Task 14, or the next chat).
- [ ] **Step 5:** Build → SUCCESS. Device: "remind me in 3 minutes to drink water, sing it" → log `line: written in N ms (W words)`, `line: recorded S s` — then lock; at its minute: beeps, the sung line, text paced with it. Listen: if the singing sounds poor, switch `styleNote("sung")` to the fallback (a `playNotes` jingle before a spoken rhyming line; the spec allows it) and ledger the ruling. A reminder and an alarm at one minute → one group recording `group-<minute>.wav`; delete one of them → the other records its own line.
- [ ] **Step 6:** Commit: `Companion: lines written and recorded ahead — in the chat, grouped by minute`.

### Task 12: Timers on Dotty

**Files:**
- Modify: `cartridges/companion/main.cpp`, `cartridges/companion/screens.h/.cpp`

**Interfaces:**
- Consumes: Task 4's `TimerState`, `timerCue`, `timerRefreshSeconds`; Task 3's `Ringing`; Task 8's `moments::timer()`.
- Produces: `screens::drawTimer(const TimerView &v)` with `struct TimerView { String label, clock; bool paused = false; bool confirmCancel = false; };`, `TimerHit screens::timerHit(uint16_t x, uint16_t y)` (`enum class TimerHit { None, Yes, No }`); in main.cpp `void pollTimer()`.

- [ ] **Step 1:** `pollTimer()` (each pass): for every whole second reached since the last pass, `timerCue(left, tickEvery, momentBusy())` → `player.setVolume(beep.volume); player.playNotes(&note, 1)` for Tick / Countdown; End → fanfare (`playNotes`: G4 C5 E5 G5 C6, 120 ms each, the last 400 ms) then the "timer" recording (or the text "Pasta: time's up"), then `Ringing::start(now, false)` (BOOT or a tap stops it).
- [ ] **Step 2:** The screen: idle state shows the timer view in place of the idle avatar while one runs; refresh by `timerRefreshSeconds` (partial; `shell::wake()` each pass in the last minute); tap = pause/resume, long press = "Cancel the timer?" Yes / No. A chat in progress keeps its screen; the timer view comes back after the reply.
- [ ] **Step 3:** Locked: the bottom line comes from `lockLine` (Task 10's `nowPlaying`); `wakeLockedAt` = the next interval tick or `endsAt − 60`; at 60 s left `shell::wake()` (unlock) and the countdown shows.
- [ ] **Step 4:** Build → SUCCESS. Device: "set a 70-second timer for the eggs" → the countdown; lock with PWR → Dotty unlocks at 60 s, ticks at 30, the last 10 s rising, the fanfare and its line, rings until BOOT; a timer ending while you hold BOOT → no countdown beeps, the end plays after the reply.
- [ ] **Step 5:** Commit: `Companion: timers — countdown screen, ticks, the rising last ten seconds, the end ringing`.

### Task 13: Bluetooth commands

**Files:**
- Modify: `cartridges/companion/main.cpp` (`registerCompanionCommands`)

**Interfaces:**
- Consumes: Task 8.
- Produces: `companion.moments {companion?}` → `{moments: [{id, companion, kind, at, repeat, days, what, style, source, ready, state}], timer?: {label, endsAt, paused, left, tickEvery}, max: 50, available}`; `companion.moment.save {id?, companion, kind, at, repeat?, days?, what, style}` → `{id}` (errors: full, past, bad kind/style, unknown companion); `companion.moment.delete {companion, id}`; `companion.moment.play {companion, id}` (fires it now as a preview, no `advance`); `companion.moment.rules {…}` → the rules (no fields = read; out-of-range values clamped); `companion.timer {action: start|pause|resume|cancel, seconds?, label?, tickEvery?}` → `{timer}`; events `companion.moments` and `companion.timer` (`void notifyMoments()`, `void notifyTimer()`). Every command answers `{ok: false, error: "unavailable"}` without a card (timer commands still work).

- [ ] **Step 1:** Implement; days as `["mon", …]`; times as local epoch numbers. A save from the app records its line right away (`pollLines` picks it up: the phone is connected).
- [ ] **Step 2:** Build → SUCCESS. Test from the Mac: `.venv/bin/python tools/ble_dotty.py cmd companion.moment.save companion=debug kind=reminder at:=<epoch + 300> what="water the plants" style=calm` → `{id}`; `cmd companion.moments` lists it with `ready` turning true after a minute (log: recorded through Wi-Fi: the Mac isn't a relay); `cmd companion.timer action=start seconds:=90 label=test` → `{timer}`; `cmd companion.moment.rules perDay:=9` → `perDay: 5`.
- [ ] **Step 3:** Commit: `Companion: moments, rules and the timer over Bluetooth`.

### Task 14: The phone wake and the daily plan

**Files:**
- Modify: `cartridges/companion/main.cpp`

**Interfaces:**
- Consumes: Task 1's `bluetoothWindow`, `wakeLockedAt`; Task 5's `PhoneWake`; Task 6's `planDue`, `planRequest`, `parsePlan`; Task 11's `pollLines` and `nextLineWork`.
- Produces: `PhoneWake phoneWake`; NVS `companion/lastWake`, `companion/plannedDay`; `void pollPhoneWake()`; `void backgroundPoll()` (the locked loop's work while a wake runs).

- [ ] **Step 1:** Jobs: **record** is due when `nextLineWork` has work; **plan** when `planDue(now, plannedDay, rules)`. Locked and `phoneWake.nextWakeAt(lastWake, wakeEvery, due)` reached (or unlocked: whenever a job is due and the phone said hello) → `phoneWake.start`, `shell::bluetoothWindow(40)`, log `phone wake: jobs record|plan`.
- [ ] **Step 2:** `backgroundPoll()` runs in `loop()` before the locked `return` while `phoneWake.stage() != Idle`: `live.poll()`, `relay::poll()`, `writerPoll`-free line work (`pollLines(false)`), `lastSavePoll()`, `pollPhoneWake()`. Phone stage: a recording session through the relay with `recordingConfig(profile of the work's companion)` when the work needs reading; Wifi stage: `shell::endBluetoothWindow()`, then the same over Wi-Fi (`connectLoop` with the recording config). The plan job: `planRequest` through `jobs` (`Job::Plan`) → `parsePlan` → each line added as a spontaneous moment (source "companion", `line` already written: its `LineWork` skips writing — `LineJob::written` given the plan's text directly) → recorded like any line; `plannedDay` saved even when the plan came back empty (once a day).
- [ ] **Step 3:** Done → close the session, `shell::endBluetoothWindow()`, `lastWake = now`, log `phone wake: done in N s via phone|wifi|nothing (R lines recorded)` (the battery measurements read this). Cancelled by an unlock + BOOT press, a firing moment or a chat (`phoneWake.end()`, `lineJob.abort()`).
- [ ] **Step 4:** Build → SUCCESS. Device: dev key `u` adds a reminder 40 minutes out without recording it (not ready) → lock → within 20 minutes `phone wake` in the log: with the iPhone near → `via phone`; with the iPhone's Bluetooth off → `via wifi`; with Wi-Fi off too → `via nothing` and Bluetooth off at 30 s. Spontaneous: `companion.moment.rules from:=<now + 40 min in minutes> to:=<now + 100 min>` → a plan wake 30 minutes before `from` → two spontaneous moments in `companion.moments`, recorded; one fires inside the window.
- [ ] **Step 5:** Commit: `Companion: the phone wake (jobs record and plan) and the daily spontaneous lines`.

### Task 15: The app's Moments screen

**Files:**
- Create: `ios/Dotty/Features/Companion/MomentsView.swift` (list, timer card, rules), `ios/Dotty/Features/Companion/MomentEditor.swift`
- Modify: `ios/Dotty/Features/Companion/CompanionView.swift` (a clock button `clock` beside the wand)

**Interfaces:**
- Consumes: Task 13's commands and events.
- Produces: `struct DottyMoment: Decodable, Identifiable` (`id` = "\(companion)-\(momentId)"), `struct MomentRulesModel: Codable`, `MomentsView(link:)`.

- [ ] **Step 1:** `MomentsView`: the timer card (label, a `Text(timerInterval:)` countdown from `endsAt` converted to a Date with the utc offset, Pause/Resume, Cancel; else "Start a timer" → a duration picker + label sheet → `companion.timer start`; a second timer asks "Replace the running timer?"); sections Today / Upcoming / Repeating / Missed (rows: kind icon `bell` / `alarm` / `bubble.left`, "Today 18:00" or "Weekdays 07:30", what, the companion's avatar image, "Recorded" or "Records at the next chat"); swipe to delete; tap → `MomentEditor`; "Play on Dotty" in a row menu; reloads on `companion.moments` / `companion.timer` events and on reconnect. Rules section: a stepper 0–5, allowed hours (two `DatePicker`s, hour and minute), quiet windows (list + editor: weekday toggles, from/to), the quiet-window choice (`ChoiceRow`s: "Beeps and text" / "Silent"), alarm volume slider 80–100, phone wake interval (10 / 20 / 30 / 60 minutes).
- [ ] **Step 2:** `MomentEditor`: kind (Reminder / Alarm), date and time, repeat (Once, Daily, Weekdays, Chosen days + toggles), what (≤ 200, a counter), style picker, companion (default the active one) → `companion.moment.save`; errors shown in words ("That time has passed", "Dotty holds 50 moments for this companion").
- [ ] **Step 3:** Build: `xcodebuild … build` → BUILD SUCCEEDED.
- [ ] **Step 4:** Commit: `App: Companion › Moments — the timer, reminders and alarms, rules`.

### Task 16: Docs, version, release

**Files:**
- Modify: `cartridges/companion/main.cpp` (`DOTTY_CARTRIDGE("companion", "Companion", "0.5.0")`), `CLAUDE.md` (a **Moments** bullet in the Companion section: kinds, files, tools, commands, the phone wake and its log line, measured numbers from Tasks 10–14; the dev keys added), `/Users/pedrosaldanha/.claude/projects/-Users-pedrosaldanha-Desktop-Dotty/memory/dotty-ai-companion.md` (piece 3 shipped)

- [ ] **Step 1:** Dev keys for the device tests (free letters; add them in the task that first needs them and list them in CLAUDE.md): `L` = a sung reminder 1 minute from now (in Task 11), `F` = fire the next moment now (Task 10), `C` = a 15-second timer (Task 12), `u` = a not-ready reminder 40 minutes out (Task 14).
- [ ] **Step 2:** `python3 tools/companion_test/run.py` → every file passes; `pio run -e companion` → SUCCESS; the app build → BUILD SUCCEEDED.
- [ ] **Step 3:** Commit, check the outgoing commits for a key prefix (`git log -p origin/main..HEAD | grep -c "AIza"` → 0), push, then `.venv/bin/python tools/build_catalog.py --release` → a release tagged latest with Companion 0.5.0.
- [ ] **Step 4:** Overnight battery check (the user's, over the following days): `B` over serial → `battery.tsv`; compare locked nights with phone wakes against the plain ones and note the result in CLAUDE.md.
