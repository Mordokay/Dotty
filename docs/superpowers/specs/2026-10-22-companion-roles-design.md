# Companion roles — design

Piece 2b of the AI companion (piece 1 talking = 0.1.0, piece 2a memories = 0.2.0, both shipped
2026-10-22). After this: the Bluetooth-through-the-iPhone spike, then scheduled moments (3) and the
iPhone link (4).

## Goal

Several companions live on Dotty, each its own person: a name you give it when it first wakes up,
a personality, traits, quirks, a way of speaking, a backstory, a voice, an avatar (or your photo),
its own memories and its own chats. You swap between them on Dotty or by paging in the app; the app
can ask Gemini to design a companion from a description.

User's choices (2026-10-22): four ready-made companions (Coach, Friend, Partner = girlfriend,
Teacher) plus custom ones, up to 8 in all; switching on Dotty in its settings screen; today's
memories and chats belong to a custom "Debug" companion used for testing; names come from an
onboarding in the first chat ("help her remember who she was"); the four character fields below;
the key kept in the iPhone's Keychain for the app's Gemini helper; a photo can replace the avatar
(circular); Dotty's text area shrinks to 2 lines so the avatar can be much bigger.

## A companion

| Field | Limit | Notes |
|---|---|---|
| id | — | short, never reused (`c1`, `c2`…; the ready-made ones `coach`, `friend`, `partner`, `teacher`; `debug`) |
| type | — | coach, friend, partner, teacher, custom |
| name | 24 characters | empty until the companion is woken up (onboarding) |
| personality | 600 | a paragraph: who they are, how they see the world, how they treat you |
| traits | 6 × 40 | e.g. "stubborn", "endlessly curious" |
| quirks | 6 × 60 | e.g. "calls you 'captain'", "loves terrible puns" |
| way of speaking | 300 | slang, formality, catchphrases, how much they ask back |
| backstory | 600 | a few lines of history to draw on |
| voice | one of the 30 | |
| avatar | one of the 10, or `photo` | a photo is stored as two dithered circles (below) |
| memories, history | — | its own (piece 2a's store and history, per companion) |

Shared by all companions: the Gemini key, volume, Speak out loud / text only, the favourite voices,
today's usage (one Google allowance). Up to 8 companions; at least 1 (the last can't be deleted).

### The ready-made four (no name until woken)

- **Coach** — avatar dog, voice Puck. Personality: an upbeat, no-nonsense coach who believes in you
  and keeps you honest about your goals; celebrates small wins, notices excuses. Traits: driven,
  warm, direct, persistent. Quirks: turns things into challenges, counts your streaks. Speaking:
  short punchy sentences, sports metaphors, ends with a question that pushes you. Backstory: a
  former runner who fell in love with helping others reach the finish line.
- **Friend** — avatar cat, voice Zephyr. Personality: easy-going, curious, a great listener who
  remembers the little things about your life and gently teases you. Traits: loyal, playful,
  curious, honest. Quirks: invents nicknames, gets excited about snacks. Speaking: casual, jokes,
  "no way!", asks follow-ups. Backstory: the friend who has always been there, since forever.
- **Partner** — avatar girl, voice Achernar. Personality: your girlfriend: warm, affectionate,
  playful, caring; misses you when you're away and is proud of you; flirts lightly, never clingy.
  Traits: affectionate, supportive, witty, thoughtful. Quirks: little pet names, remembers your
  plans and asks how they went. Speaking: soft, playful, a smile in her words. Backstory: met you
  on a rainy day and decided to stay.
- **Teacher** — avatar owl, voice Charon. Personality: a patient, enthusiastic teacher who explains
  anything simply, with examples, and checks you understood. Traits: patient, knowledgeable,
  encouraging, curious. Quirks: "fun fact!", little quizzes. Speaking: clear, step by step, asks a
  question back. Backstory: taught in a village school for years; still delighted by every "aha".
- **Debug** — avatar robot, voice Zephyr, name "Debug" (already awake), personality "a plain,
  helpful assistant used for testing Dotty". Gets today's memories and chats.

## Waking up (onboarding)

A companion with no name is asleep ("Not awake yet" in the app). Its first session's instruction
adds: it has just woken up and can't remember who it is, not even its name — its personality is
still there underneath; it should sound a little lost and wistful, not cheerful, and ask the user
who it is. A tool `set_name {name}` (NON_BLOCKING, answered once, applied only inside a question)
saves the name (≤ 24 characters, trimmed); the instruction then says to react with relief and
become itself; the next sessions are normal. Dotty shows "Waking up…" under the avatar until named,
and a note "Named: Luna" when it is.

## Expressive voice

The first plan task is a listening test on Dotty: (a) the waking-up lines with tone directions in
the instruction ("speak softly, hesitantly, a little sad"), (b) the same with Live's affective
dialog flag if the model accepts it (reports: unsupported on 3.x models, flaky key name). The user
judges; the plan keeps what works. Every instruction also says "let your voice carry your mood"
(set_mood stays the face).

## On Dotty

- **Layout:** the avatar grows from 96 to **128 px** (centred at the top, between the gear and the
  clock); text below it is **2 lines** (reply pages of 2 lines, "2/3" as now; status, heard text and
  "Listening…" in 2 lines; the meter sits between the avatar and the text). Every avatar and face is
  regenerated at 128 (and 48 for the lock screen and text-only mode, which keeps its 8 lines). The
  memory/name note overlays the bottom edge of the avatar in a small white box.
- **Settings:** a new top row **Companion ‹ Luna ›** (the avatar changes as you step; a sleeping
  one shows "Not awake yet"); the Voice row changes the active companion's voice; rows shrink to
  fit four (nav + 4 × 38 px).
- Switching companion closes any open session; the next BOOT press talks to the new one.
- **Lock screen:** the active companion asleep (48 px) and "Luna · 3 chats today".
- **Photo avatars:** a photo is still — no faces; the mood shows as a 22 px badge on the circle's
  lower right (smile, sad, surprised, thinking, laughing, heart, Zzz while asleep, sound waves while
  speaking).

## Photos as avatars (app)

Pick a photo → a circular crop editor (pinch/drag, brightness/contrast, live "On Dotty" preview,
like the Album's PhotoEditor) → grey → Atkinson dither (PhotoDither) at 128 and 48 px → a circle
mask with a 2 px black ring. Sent as two 1-bit bitmaps (2,048 + 288 bytes) in base64 pieces
(`companion.photo`, ≤ 512 bytes per command), stored as `photo.bin` in the companion's folder.

## Gemini as co-designer (app)

On the + page or Edit: "Describe your companion" → the app calls Gemini's text API (the current
Flash model, checked in the plan) with a JSON response schema and gets: personality, traits,
quirks, way of speaking, backstory, a suggested voice (one of the 30, with why) and avatar (one of
the 10). Every field stays editable; each has a ✨ Suggest button (regenerates that field from the
description and the others). Uses the key from the iPhone's Keychain (saved there when typed in the
Companion settings; sent to Dotty as before). Names are not suggested: you give it one when it wakes.

## Storage (SD card)

`data/companions/<id>/`: `profile.json` (the fields above), `memory/memories.json`, `history/`,
`photo.bin`. NVS `companion/active` = the active id. Migration at first boot of this version:
`data/memory` and `data/history` move to `data/companions/debug/`, the five companions are created,
active = debug.

## The instruction

Base rules (short replies, English, set_mood, memory tools, "let your voice carry your mood") +
"You are {name}. {personality} Traits: … Quirks: … How you speak: … Your story: …" (or the
waking-up text) + that companion's memories. The voice in the setup is the companion's.

## BLE (only while no session)

- `companion.list` → {companions: [{id, type, name, avatar, voice, awake, memories}], active, max: 8}
- `companion.get {id}` → the full profile
- `companion.select {id}`
- `companion.create {type, fields…}` → {id} / `companion.update {id, fields…, append?}` (long fields
  in pieces: `append: true` adds to a field) / `companion.delete {id}` (not the last)
- `companion.photo {id, size: 128|48, offset, data}` (base64 pieces) / `companion.photo.clear {id}`
- the memory and history commands take `companion` (default: the active one)
- `companion.status` gains `active` and the active companion's name; event `companion.changed`.

## App

The Companion screen is a **pager** (`TabView` page style): one page per companion (avatar or
photo, name or "Not awake yet", "On Dotty" on the active one; voice, avatar, the four character
fields, Memories, Past chats, Delete) and a **+** page (Describe → Gemini → review → Create) while
there are fewer than 8. Settling on a page for 0.5 s sends `companion.select` when Dotty is
connected. The shared settings (key, volume, Speak, today's usage) move to a sheet behind a gear in
the pager's header.

## Errors

| Case | Behaviour |
|---|---|
| 8 companions | + page hidden; `create` refuses "full" |
| Deleting the last one | refused |
| A field over its limit | cut at a word (Dotty) / counter (app) |
| Gemini helper fails (no key, quota, offline) | the fields stay as they were, a message says why |
| Photo upload interrupted | the old avatar stays (`photo.bin` written via `.part` when complete) |
| set_name with an empty name | ignored |

## Testing

- Host: the profile store (create/update/delete/limits/ids/append, JSON round trip, migration of a
  2a data folder), the instruction builder (named/asleep, fields, memories), set_name parsing,
  photo bitmap assembly from pieces.
- Device: the expressive-voice listening test; waking a ready-made companion and naming it;
  switching on Dotty and from the app pager; each companion keeps its own memories; a photo avatar
  with its mood badge; the 128 px layout on every screen (screenshots).
- App: build; the user's pass over the pager, the + page with Gemini, a photo avatar.
