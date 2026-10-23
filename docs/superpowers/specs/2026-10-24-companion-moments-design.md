# Companion scheduled moments — design

Piece 3 of the companion roadmap, after the relay (0.4.0) and 300-word answers (0.4.1). Next: the
outside world through the iPhone (piece 4: the phone sync bundle, ANCS notifications, HealthKit,
calendar, Slack, Reddit), which plugs into the phone wake built here.

## Goal

The companion speaks first at the right moment: a reminder you asked for, an alarm that gets you up, a
timer that counts down, or a line of its own during the day ("I keep thinking about your surf trip").
Moments are set by talking ("remind me at 6 pm to cook dinner, sing it") or in the app. They play when
Dotty is locked, even with no phone or internet nearby, because each line is recorded ahead of time.

## User's choices (2026-10-24)

- Kinds: **reminder**, **alarm** (rings until you answer, snooze), **timer**, and **spontaneous** lines
  from the companion itself, within a daily allowance and allowed hours. Quiet windows (work hours,
  do-not-disturb).
- Before the companion speaks, **4–5 rising beeps** to say "pay attention", then the line.
- Alarms repeat until a button is pressed; tap = snooze, with a limit, then it gives up.
- Lines are **prepared ahead** (option 1) and kept in a temporary folder on the card; styles include
  sung ("remind me at 6 pm to cook my dinner in a song").
- Timers: "start a 10-minute timer"; beeps at regular intervals; the last 10 s a beep a second, with
  pitch and volume rising.
- The **phone wake** (Dotty turns Bluetooth on for a short window while locked so the app can reach it)
  is built as a **general mechanism with a list of jobs**: here its only job is recording lines; the
  phone sync bundle joins it in piece 4. Interval **20 minutes** to start with (1000 mAh battery; to
  be measured over the coming weeks).
- General rule: heavy lifting runs on the iPhone when it's there, but Dotty works on its own without
  any phone or app.
- Privacy: personal data to Google is fine; never passwords, bank or card data.

## What a moment is

A moment = when + what + how, owned by one companion.

```
{ id, kind: reminder|alarm|timer|spontaneous, at (local epoch), repeat?: daily|weekdays|days,
  days?: [mon..sun], what (≤ 200 characters: what it's about), style: spoken|sung|whispered|
  excited|calm, source: chat|app|companion, line? (the text to say, once written), ready (recorded),
  state: upcoming|ringing|snoozed|done|missed, snoozes, created }
```

- `source` names where a moment came from. Later sources (calendar, slack, notifications…) add values
  here and nothing else changes in how moments fire.
- Stored per companion in `data/companions/<id>/moments.json` (via `.part`, like the other stores; a
  broken file → `moments.bad.json`, empty list), at most **50 per companion**; ids never reused.
- Recordings: `data/companions/<id>/moments/<momentId>.wav` (24 kHz, 16-bit) + `<momentId>.json`
  (the line's text pieces timed to the audio, the replay's `last_line` format, so the screen pages the
  text in time). Deleted when a one-off moment is done or missed; a repeating moment keeps its
  recording. Boot deletes recordings with no moment.
- **Rules**, shared by every companion, in `data/moment_rules.json`:
  - `perDay` (spontaneous lines a day, 0–5, default 2), `from`/`to` (allowed hours, default
    10:00–21:00);
  - `quiet`: a list of windows `{days, from, to}` (work hours, do-not-disturb), none by default;
  - `quietMode`: what a reminder does in a quiet window: `beeps` (beeps + text, default) or `silent`
    (text only);
  - `alarmVolume` (≥ 80 %, default 90 %);
  - `wakeEvery` (the phone wake interval, minutes, default 20).
- One **timer** at a time (shared, not per companion; it belongs to the companion that started it for
  its closing line): `{label, endsAt, paused, left, tickEvery, companion, line?, ready}`, kept in NVS
  `companion` (`timer*` keys) so a restart keeps it.

## How a moment fires

- Moments only fire while the Companion cartridge runs (another cartridge or power off = "missed while
  off", below).
- The scheduler (`moment_schedule.*`, plain C++) gives the next due moment across companions. Locked,
  the shell's minute wake checks it, so a moment fires at its minute. A timer needs a wake at a given
  second (60 s before it ends): a small shell addition, `shell::wakeLockedAt(time_t local)`, makes the
  next sleep end then if that's sooner than the next minute.
- **Firing**: Dotty unlocks (`shell::wake()`: the screen lights up like an alarm clock; Bluetooth comes
  on as with any unlock) and shows the **moment screen**: the companion's avatar, a kind icon (bell,
  alarm clock, timer, speech bubble), the line paged 3 lines at a time. Then the **attention beeps**
  (5 rising notes, ~2 s, `playNotes`), 0.5 s of quiet, then the recording, with the text paced by the
  audio like a reply. It auto-locks after the usual 2 minutes.
- **Reminder and spontaneous**: play once. Holding BOOT within 3 minutes opens a chat whose instruction
  says what the companion just said ("You just reminded them: …"), so "thanks, what should I cook?"
  makes sense.
- **Alarm**: plays at `alarmVolume`, ignoring quiet windows and mute; repeats (beeps + line) every
  minute for up to 10 minutes. **BOOT** = "I'm up" (done; one sentence on screen, no chat opened).
  **Tap** = snooze 5 minutes, at most 3 times (the 4th tap counts as BOOT). After 10 minutes of ringing
  with no answer: it stops, and the lock screen shows "Missed alarm 07:30" until the next unlock.
- **Timer**: see "Timers" below.
- **Quiet windows**: reminders follow `quietMode`; spontaneous lines are never placed in them; alarms
  and timers ignore them.
- **Busy**: a moment due during a chat (recording or a reply playing) waits until the reply ends; while
  Dotty is listening (BOOT held) it waits until the reply to that has ended.
- **Without a recording** (preparation failed): beeps + the text on screen (`line`, or `what` if no line
  was written).
- **Missed while off** (another cartridge or powered off): at start, a moment due within the last hour
  fires now; an older one becomes `missed` (shown in the app and, for alarms, on the lock screen).
  Repeating moments then move to their next time.
- Every fired, done or missed moment sends `companion.moments` when the phone is connected and adds a
  line to the companion's history (`{you: "", dotty: line, moment: kind}`), so the app's chat and recall
  see it.

## Making the lines

Every line goes the same way: **the writer writes it, Live reads it, Dotty records it**. That makes the
text exact (shown on screen), the voice the companion's own, and the length controlled, which is the
path proven by the writer and macros.

1. **Write**: the writer (`gemini-flash-lite-latest`) gets the companion's character and memories, the
   moment (`kind`, `what`, `style`, the time it will play) and returns the line, **≤ 150 words**
   (about a minute: the length the writer and word-for-word reading are proven at, the user's choice),
   as short as the message allows: a plain reminder is usually a sentence or two. Through `relay.fetch` when the phone is there, else Dotty's own HTTPS.
2. **Read**: a Live turn "say the text below aloud exactly, word for word, {style note}" (style notes:
   sung → "sing it to a simple, cheerful melody"; whispered, excited, calm → the matching delivery).
   A line over ~100 words is read in chunks like a long answer
   (`readingChunks`, 0.4.1); the recording has no gaps between them. The reply audio goes to an
   `AdpcmRecorder`, not to the speaker, then the WAV + timing file are saved
   by the replay's save task.
3. **Singing is an approximation**: Live is a speech model. The first device test decides; if it sounds
   poor, `sung` becomes a short `playNotes` jingle before a spoken, rhyming line (the spec's fallback,
   no further approval needed).

**When**:
- **In a chat** ("remind me at 6"): the companion calls `set_moment`; once the current reply has ended,
  Dotty writes and records the line on the same session (screen note: a bookmark + "Saving your
  reminder..." for the few seconds it takes). Pressing BOOT meanwhile takes priority: the recording is
  dropped and retried at the end-of-chat review, before the session closes.
- **From the app**: the app can only send it while connected, so the phone is there: Dotty records
  right away through the relay (if a chat is running, after it).
- **Spontaneous**: a daily **planning job** (below) 30 minutes before the allowed hours start: the
  writer gets the companion's memories, recent summaries and today's date, and returns today's lines
  as JSON: `perDay` items `{at, what, line, style}` at least 2 h apart, inside the allowed hours,
  outside quiet windows (Dotty checks and drops any that aren't). Kinds of line: a callback to something
  you said, a question about your plans, a joke, "I miss you". The active companion only. Then each is
  read and recorded.
- **Not ready** (quota, network, no phone and no Wi-Fi): retried at every phone wake until the moment
  fires (then the no-recording fallback).
- Cost: a short line ≈ 2–3k tokens (writer + reading), a full 150-word line ≈ 6–7k; a day of 2
  spontaneous lines ≈ 6–14k. A 150-word recording is ~60 s, ~2.9 MB on the card.

## Timers

- Started by voice, `start_timer {seconds, label?, tickEvery?}` (≤ 24 h), or from the app
  (`companion.timer`). One at a time: `start_timer` while one runs answers `{error: "a timer is running", label, left}`
  and the model asks whether to replace it (then `stop_timer` + `start_timer`); the app asks the same
  in a confirmation.
  `stop_timer` cancels it.
- Its closing line ("Time's up, the pasta's ready!") is written and recorded like a moment's, while it
  runs. If it isn't ready when the timer ends: a fanfare and "Pasta: time's up" on screen.
- **Screen**: unlocked, a timer screen in place of the idle avatar: the label and a big M:SS, refreshed
  every 10 s (partial), every second in the last minute; tap = pause / resume, long press = cancel
  ("Cancel the timer?" Yes / No). BOOT still talks (the countdown carries on; the timer screen comes back
  after the reply). Locked: the lock screen's bottom line "Pasta · 7 min", once a minute; at 60 s left
  Dotty unlocks and shows the countdown.
- **Sound plan** (`timer_beeps.*`, plain C++, host-tested: given the seconds left, what to play):
  - a soft tick every `tickEvery` seconds if set (none by default), not in the last minute;
  - one tick at 60 s and one at 30 s;
  - the last 10 s: a beep each second, each a semitone higher, volume rising in even steps from 50 % to
    100 % (each beep its own `playNotes` call after `setVolume`);
  - at 0: a short fanfare, then the recorded line.
- **Ringing** after it ends: like an alarm (fanfare + line every minute for up to 10 minutes, at
  `alarmVolume`), but BOOT **or** a tap stops it; no snooze.
- **During a chat**: interval ticks and the last-10-s beeps are skipped while Dotty listens or speaks;
  the end plays once the current reply has ended.

## The phone wake

A general mechanism for reaching the phone while Dotty is locked (`phone_wake.*` in the companion;
the window itself in the shell).

- **Shell**: `shell::bluetoothWindow(seconds)` turns Bluetooth on while locked (with a wake lock) until
  the time runs out or `shell::endBluetoothWindow()`; then off again so Dotty sleeps. It lives in the
  shell because the shell owns Bluetooth while locked; any cartridge may use it.
- **Jobs**: each job says whether it has work (`due(now)`), runs as a small state machine once the
  window is open (`poll`), and reports when it's done. Piece 3 has two jobs:
  - **record**: lines not ready (moments, the timer's line);
  - **plan**: today's spontaneous lines, once a day, 30 minutes before the allowed hours.
  Piece 4 adds **sync** (the phone's bundle), which is always due.
- **When**: every `wakeEvery` minutes (20) while locked, **only if a job is due** (piece 3 never wakes
  for nothing); on unlock the jobs run anyway when Bluetooth is up and the phone says hello.
- **Window**: Bluetooth on; up to **30 s** for the phone to reconnect and send `relay.hello`; then the
  jobs run through the relay (each Live session or fetch as in a chat), up to **3 minutes** in all; then
  Bluetooth off. No phone in 30 s: jobs that can use Wi-Fi (record, plan) do so with Bluetooth off (as a
  Wi-Fi chat); jobs that need the phone (sync) wait for the next wake.
- The lock screen doesn't change during a wake (no flashing). A wake never runs during a chat or while
  a moment rings.
- Battery: ~30 s of Bluetooth plus, sometimes, a short session per wake. Measured with `battery.tsv`
  over a few days (each wake logs its length and path).

## Tools (Gemini Live)

- `set_moment {kind: reminder|alarm, at: "YYYY-MM-DDTHH:MM" (local), repeat?, days?, what, style?}` →
  `{id, at}` or `{error: "in the past" | "full" | …}`. The instruction carries the date and time
  (`nowText`), so "at 6 pm" and "in 20 minutes" become times.
- `cancel_moment {id}`.
- `list_moments {}` → the companion's upcoming moments as short text (answered with content, like
  `recall`). The instruction itself lists only how many there are and the next 3, to save tokens.
- `start_timer {seconds, label?, tickEvery?}`, `stop_timer {}`.
- Each is NON_BLOCKING, answered once (TurnTracker), applied only inside a question.
- The instruction tells the model: confirm a moment briefly in its own words ("Done, I'll sing it to
  you at six"); never set moments the user didn't ask for (spontaneous ones are Dotty's job, not the
  model's).

## App: Companion › Moments

A clock button next to the wand and the gear.

- **Timer card** at the top while one runs: label, countdown, Pause / Cancel. Otherwise "Start a timer":
  a duration picker and a label.
- **List**, grouped Today / Upcoming / Repeating / Missed; a row = kind icon, time ("Today 18:00",
  "Weekdays 07:30"), what, the companion's avatar, "Recorded" or "Records at the next chat". Swipe to
  delete; tap to edit (a changed `what` or `style` records again).
- **New moment**: kind (Reminder / Alarm), date and time, repeat (once, daily, weekdays, chosen days),
  what (≤ 200), style, companion (the active one).
- **Rules**: spontaneous lines a day (0–5), allowed hours, quiet windows (add / edit / delete: days +
  times), in a quiet window (beeps and text / silent), alarm volume, phone wake interval.
- "Play on Dotty" per row (no preview on the phone: the recordings live on the card).

## Bluetooth commands

Local epoch times, every command ≤ 512 bytes.

- `companion.moments {companion?}` → `{moments[{id, kind, at, repeat?, days?, what, style, companion,
  source, ready, state}], timer?, max}` (all companions when none is named; long replies in pieces as
  today).
- `companion.moment.save {id?, companion, kind, at, repeat?, days?, what, style}` → `{id}`.
- `companion.moment.delete {companion, id}`, `companion.moment.play {companion, id}`.
- `companion.moment.rules {perDay?, from?, to?, quiet?, quietMode?, alarmVolume?, wakeEvery?}` → the
  rules (no fields = read).
- `companion.timer {action: start|pause|resume|cancel, seconds?, label?, tickEvery?}` → `{timer}`.
- Events `companion.moments`, `companion.timer`.

## Failures

| Case | Behaviour |
|---|---|
| Writing or recording fails | not ready; retried at each phone wake; fires as beeps + text if still not ready |
| No phone and no Wi-Fi at a wake | jobs wait for the next wake |
| Quota out ("out of words") | no recording until the reset; moments still fire as beeps + text |
| A moment in the past from the tool | `{error: "in the past"}`, the model asks again |
| 50 moments | `{error: "full"}`; the app says so |
| Card missing | moments unavailable (`{ok: false, error: "unavailable"}`, like memories); a running timer still works |
| Dotty off at the time | ≤ 1 h late: fires; older: missed |
| Clock changes (core.time, SNTP) | the scheduler works from local epoch, so moments follow the new time; a jump past a moment counts like being off |
| Sung style sounds poor | the jingle + spoken rhyme fallback |

## Out of scope

The phone sync bundle, ANCS notifications, HealthKit, calendar, Slack, Reddit (piece 4 and later, as
phone wake jobs and moment sources); several timers at once; moments triggered by place or by
something other than time; sharing a moment between companions.

## Testing

- **Host** (`tools/companion_test/`): `moment_store` (load / save, 50 per companion, ids, broken file);
  `moment_schedule` (next due across companions, repeats incl. chosen days, quiet windows and
  `quietMode`, the ≤ 1 h late rule, alarm repeats for 10 minutes, snooze count to 3, clock jumps);
  `timer_beeps` (ticks, 60 / 30 s, the last 10 s with pitch and volume, none during a chat);
  `phone_wake` job choice (no job due = no wake, the 20-minute interval, the 30 s / 3 min limits,
  Wi-Fi fallback per job); the spontaneous plan's JSON parsing and its checks (hours, quiet windows,
  2 h apart); the new tools' parsing and replies.
- **Device**: "remind me in 2 minutes" then lock → fires with beeps and its line; a sung reminder (the
  singing check); an alarm through 3 snoozes and giving up into "Missed alarm"; an app-made reminder
  recorded through the phone, and one recorded over Wi-Fi with the phone away; a spontaneous day with
  the allowed hours squeezed into the next hour; a 1-minute timer (the last 10 s, the ringing); a timer
  ending mid-chat; Dotty switched to another cartridge across a moment (late vs missed); overnight
  `battery.tsv` with phone wakes vs plain locked nights.
- **App**: build; the user's pass on the Moments screen.
