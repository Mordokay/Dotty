# Companion recordings — design

Piece 3b of the companion roadmap, after scheduled moments (0.5.x). Next: 3c GPS directions, then the
outside world through the iPhone (piece 4). Also in this piece: the app's companion pager as avatar
circles, and two small screen changes on Dotty (a timer button, a cancel on the recording deck).

## Goal

Tell a companion "I want to record something about eating bananas in space", talk for as long as you like
(holding BOOT, pausing between thoughts), and the companion keeps it: the audio on Dotty and on the
iPhone, an exact transcript, a short summary with key points. It reacts to what you actually said, can
play it back on request, and knows it afterwards — in every chat through the summaries, word for word
through recall.

## User's choices (2026-10-24)

- Every recording is **copied to the iPhone automatically**; the audio **streams to the phone while you
  record**, so the companion reacts within seconds of Done.
- **Transcripts are made on the iPhone and copied back to Dotty as text**: the companions must know what
  was said, and text is the best form for that.
- Each recording gets a **summary with bullet points** (what the companion uses most of the time); the
  **full transcript** stays the precise record for recall ("what was that line I said?").
- The companion may **save memories** from a recording, as in a chat.
- **16 kHz**, **30 minutes** at most per recording.
- **Voice only**: started and played back by asking; browsed in the app. No recordings list on Dotty.
- **Accent**: the user is fluent but not native; Siri often fails them. Apple's on-device recognition gives
  the live draft; **Gemini makes the final transcript** from the audio. **Both are kept and shown side by
  side** in the app at first, to see how well Apple does.
- Recordings live in the companion's own folder (the sandbox rule), never Tape's; the recorder is the
  companion's own (Tape is untouched).
- Heavy work on the iPhone; Dotty still works without it (the recording is kept and catches up later).

## Recording on Dotty

- **Start**: the companion calls `start_recording {title}` and answers in its own words ("Great, start
  whenever you're ready"). When that reply ends, Dotty opens the **recording deck**.
- **The deck** (full screen): a nav bar (back arrow = cancel; the title); before the first hold "Hold BOOT
  to record"; while recording a big ● REC, the elapsed time and the level meter; paused, PAUSED with the
  time and a row ↶ Undo (drops the last part) · ✓ Done · 🗑 Discard ("Discard this recording?" Yes / No).
  BOOT held = record, released = pause; each hold is a part. Nothing goes to Gemini.
- **Cancel** (back arrow): nothing recorded → closes at once; something recorded → "Discard this
  recording?" Yes / No. Either way back to the companion's screen.
- Dotty stays unlocked while the deck is open; **locking (or the auto-lock) = Done**.
- **Limits**: nothing recorded within 2 minutes → the deck closes, nothing kept; 5 minutes paused untouched
  → Done; 30 minutes recorded → a beep at 29:30, Done at 30:00. "No room on the card" (< 50 MB free) → the
  deck doesn't open and the companion is told so.
- **The recorder** (`voice_note.*`, the companion's own, on its existing 16 kHz capture): each 100 ms
  chunk goes to (1) a WAV on the card through a writer task (a pause writes the header length, so a power
  cut keeps everything up to the last pause; boot repairs a header that disagrees with the file), (2) the
  phone as ADPCM while it's connected and ready, (3) the meter. Undo truncates the file to the last
  part's start (VFS `truncate`).
- **After Done**:
  - With the phone: Dotty waits up to 10 s for the transcript (the live draft, below), then sends the
    companion a typed note it doesn't show — title, length, transcript — asking it to say the recording is
    saved and react to what was said (it may call `remember`). The session reopens if it had closed
    (0.5 s through the phone).
  - Without the phone (a Wi-Fi chat): the companion says it's saved and that it will listen properly once
    the phone is near. The rest happens at the next phone wake.
  - A history line: `{you: "(recording) Bananas in space · 2:14", dotty: <reaction>, recording: <name>}`.

## The iPhone side

- **Streaming** over the relay link (the phone must have said `relay.hello`): Dotty → phone events
  `rec.begin {id, companion, title}`, `rec.part {id, n}` (a hold starts), `rec.undo {id}`, `rec.end {id,
  how: done|discard}`, and audio notifications `0x1F 0x02 seq` + an ADPCM frame (16 kHz, 570 samples with
  their own start state, as the relay's voice frames; `0x01` stays the chat's voice).
- **The app** (`RecordingService`, app-wide like `RelayService`, works in the background with a background
  task per recording): keeps the parts as WAV in `Documents/Recordings/<companion>/<id>.wav` (undo drops
  the last part), and feeds Apple's speech recognition (`SFSpeechRecognizer`, `requiresOnDeviceRecognition`,
  English) live: the **draft**.
- **First plan step (a probe)**: does live on-device recognition run with the app in the background and
  the iPhone locked? If not, the draft is made from the file when the app can (or skipped), and the
  reaction waits for the final transcript.
- **After Done** (on the phone):
  1. **Final transcript (Gemini)**: recordings up to 10 minutes — the audio (16 kHz, base64) to the text
     model (`kWriterModel`, `generateContent`) with the draft, the companion's memories (names, places)
     and the title as hints: "transcribe exactly what was said; keep their words; fix only mishearings".
     ~2k tokens a minute. Longer ones — the text model corrects the draft against the memories and context
     without the audio (a few hundred tokens; it can't recover words Apple missed).
  2. **Summary**: one or two sentences plus 3-6 bullet points, and a better title when the given one is a
     placeholder.
  3. Back to Dotty: `companion.recording.text {id, companion, transcript, isDraft, summary, points[], title?}`
     (Apple's draft stays on the iPhone for the comparison; Dotty keeps one transcript)
     — long text in pieces (`companion.recording.piece {id, field, offset, total, text}`, 240-byte pieces,
     like the macros' prompts).
- **Quota out** for the day: the draft is used, marked "draft" in the app, and upgraded the next day.
- **Catching up — the `sync` phone-wake job** (moments' phone wake gains it): recordings Dotty holds that
  the phone lacks (away, joined late, a gap in the frame numbers) are sent whole as ADPCM (~20 s of
  Bluetooth per minute) when the phone asks `rec.fetch {id}`; the phone then makes the draft, the final
  transcript and the summary, and sends the text back. Dotty marks a recording `synced` and `text` in its
  details; nothing on Dotty is deleted because it reached the phone.

## Storage and what the companion knows

- **Dotty**, `data/companions/<id>/recordings/`: `<YYYYMMDD-HHMMSS>.wav` (16 kHz, ~2 MB a minute), `.json`
  {title, created, ms, parts, synced, textAt, isDraft, summary, points[]} and `.txt` (the final transcript,
  else the draft until the final one comes). Never deleted by themselves.
- **In every chat**: the latest 10 recordings' summaries (date, title, summary; points left out to save
  tokens) join the instruction next to the chat summaries.
- **recall** also searches transcripts (by date and topic, as chats), so a single line can be found word
  for word.
- **Tools**: `start_recording {title}`; `play_recording {query}` (a title word, a date, "the last one";
  Dotty picks the best match by title, date and summary and plays it; "not found" otherwise);
  `delete_recording {query}` (only after the user confirms in the chat — the instruction says so).
- **BLE**: `companion.recordings {companion}` → {recordings[{name, title, created, ms, synced, text,
  summary}]}, `companion.recording.text`, `.piece`, `.rename {companion, name, title}`, `.delete
  {companion, name}`, `.play {companion, name}`; event `companion.recordings`.

## App

- **Recordings** row on each companion's page → a list, newest first: title, date, length, summary, "On
  iPhone ✓" / "Waiting to sync", "draft" when only Apple's text exists.
- A recording's page: play (the iPhone's copy), the summary and points, the transcript (selectable) with
  **Final / Apple** tabs to compare the two, share (audio or text), rename, delete (both copies, with a
  confirmation), Play on Dotty.
- **Companion pager**: the dots become a row of 28 px circles with each companion's avatar (or photo), a
  ring in one of the design system's colours (firefly, leaf, lagoon, dusk, bloom, ember, amber, by
  companion order); the page shown has a full ring, the others dimmed with a thin one; the companion active
  on Dotty has a small dot under it; the last circle is a dashed "+". Tapping a circle jumps to its page;
  swiping still works.

## Dotty screen changes

- **Timer button**: while a timer runs, the companion's main screen keeps its look and shows a small
  hourglass with the time left ("7:12") in a corner; a tap opens the timer screen, which gets a back arrow
  (to the companion). Tap = pause / resume and a long press = cancel, as before. In the last minute the
  timer screen opens by itself (Dotty unlocks for it); back still returns.
- **Recording deck cancel**: the back arrow above.

## Failures

| Case | Behaviour |
|---|---|
| Phone away while recording | kept on Dotty; copy, transcripts and summary at the next phone wake |
| Phone drops halfway | recording carries on to the card; the sync job fills the gap |
| Live recognition refused in the background | the draft from the file later; the reaction waits for the final transcript |
| Gemini out of words | the draft is the transcript (marked), upgraded the next day |
| Card full (< 50 MB) | the deck doesn't open: "No room on the card" |
| Power cut while recording | everything up to the last pause survives; the header repaired at boot |
| A frame lost on Bluetooth | the phone sees the gap and asks for the whole file (sync) |

## Out of scope

A recordings list on Dotty; a start button on Dotty; recording music in high quality (32 kHz); editing
the transcript in the app; sharing recordings between companions; other languages than English.

## Testing

- **Host** (`tools/companion_test/`): the recordings store (details JSON, names, the instruction's
  summaries section), `play_recording`'s matching (title, date, "last"), the deck's state machine (holds,
  parts, Undo, Done, Discard, cancel, the 2 / 5 / 30-minute limits), the `rec.*` events and the 0x02 frame
  header.
- **Swift checks** (`tools/relay_swift_check/`): frame order and gap detection, undo on the phone.
- **Device**: a recording with the phone (an instant reaction); with the iPhone locked in a pocket (the
  background probe); with the phone away (sync later); Undo, Discard, cancel; the user's accent — Apple's
  draft vs Gemini's final, side by side; recall of a single line; play by title and by "the last one"; the
  30-minute limit (lowered for the test); the timer button and back.
- **App**: build; the user's pass on the list, a recording's page, the pager.
