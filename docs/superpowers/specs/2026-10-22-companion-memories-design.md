# Companion memories — design

Piece 2a of the AI companion (piece 1, talking, shipped as Companion 0.1.0 on 2026-10-22). Roles
(coach, friend, partner, teacher, custom) are piece 2b, not part of this.

## Goal

The companion remembers what matters about you across chats, uses it whenever it helps, decides
by itself what is worth keeping, and you can see, add, edit and delete its memories in the app.

User's choices (2026-10-22): it saves by itself (no "shall I remember?"), it knows every memory
from the start of a chat (no lookups), memories sit in a few fixed groups.

## What a memory is

- One short sentence, at most 200 characters ("Your sister is called Ana", "You hate mornings",
  "Dentist on 3 November at 10:00").
- A group: `you` (About you), `people` (People), `likes` (Likes & dislikes), `plans` (Plans &
  dates), `other` (Other).
- An id (a number that never repeats), who added it (`dotty` or `app`), when it was added and
  last changed (local epoch seconds).
- At most 300. When full, Dotty refuses a new one (the companion is told so and says it; the app
  shows "Memory full: delete some first").

## Storage (SD card)

`/cartridges/companion/data/memory/memories.json`: `{"next": 43, "memories": [{"id", "group",
"text", "by", "added", "changed"}…]}`, written to `memories.json.part` then renamed (a power cut
keeps the old file). ~300 × 250 B = ~75 KB, parsed into PSRAM. Deleting the companion's data
(storage.remove with data) deletes it with everything else. Factory reset formats the card.

## Recall: every memory in the chat

When a session opens, the system instruction gets a section after the existing text:

> What you remember about the user (use it naturally, don't recite it; numbers are for the
> memory tools):
> About you: [3] Lives by the sea in Lisbon. [7] Swims every morning before work.
> People: [12] Sister Ana, lives in Porto.
> …

Groups in a fixed order, oldest first within a group. Cost: ~25 tokens per memory on every turn
(each turn re-reads the session); 100 memories ≈ +2,500 tokens a turn. The 300 cap bounds it.

## Saving: three tools

Declared beside `set_mood`, all NON_BLOCKING (the reply never waits for them):

- `remember {text, group}` → a new memory.
- `update_memory {id, text, group?}` → replaces one (e.g. you moved city).
- `forget_memory {id}` → deletes one ("forget that").

The instruction says what is worth remembering: names and relationships, preferences, plans and
dates, goals, health or life details you share, things you ask it to remember. Not small talk,
not one-off questions, not what's already remembered (update instead), never passwords, codes or
other secrets. Text in the second person ("You…"), short, standalone.

Tool responses: the set_mood loop (2026-10-22: each answer made Gemini speak again) means the
first plan task tests on the device how a remember call behaves (answered once, or not at all)
before the rest is built; TurnTracker's runaway guard stays. A memory call outside a question
(nobody asked) is not applied.

On Dotty: a saved, changed or forgotten memory shows a small bookmark icon and "Remembered" /
"Updated" / "Forgotten" under the avatar for ~3 s (no other memory UI on Dotty). The day's history
line gets the memory change too (`"memory": "remembered: Your sister is called Ana"`).

## The app

Companion screen › **Memories** (row with the count) → MemoriesView:

- Search field; memories by group (sections in the fixed order), newest first; "by Dotty" /
  "by you" and the date in small text.
- Tap → edit sheet (text field with the 200-character limit, group picker, Save / Delete).
- Swipe to delete; "Add a memory" (same sheet); "Forget everything" (confirmation).
- Reload on `companion.changed`.

BLE (only while no session, like the other companion commands):

- `companion.memories {offset?, limit? = 50}` → {memories: [{id, group, text, by, added,
  changed}], total, max: 300} (paged: a full list is ~75 KB).
- `companion.memory.add {text, group}` → {ok, id} or {ok: false, error: "full" | "empty"}.
- `companion.memory.update {id, text?, group?}`, `companion.memory.delete {id}`,
  `companion.memory.clear`.
- Each change sends `companion.changed`. `companion.status` gains `memories` (the count).

Changes from the app apply from the next chat (Bluetooth is off during one).

## Errors

| Case | Behaviour |
|---|---|
| Card missing or the file unreadable | Chats work without memories; the app shows "Memories unavailable". A broken file is kept as `memories.bad.json` and a new one started. |
| Memory full | remember refused; Dotty's companion is told; the app says "Memory full". |
| Text over 200 characters | cut at a word boundary (Dotty) / refused with a count (app). |
| Unknown id in update/forget | ignored (logged). |

## Testing

- Host (`tools/companion_test/`): the memory store (add, update, delete, cap, next id, group
  validation, the 200-character cut, JSON round trip, a broken file), the instruction section
  (grouping, order, numbers, empty = no section), the three tool declarations and parsing their
  calls from captured samples.
- Device: the tool-response behaviour test (first plan task); say "my sister is called Ana" →
  "Remembered" on Dotty, it's in the app; new chat → "what's my sister's name?" → Ana; "forget my
  sister's name" → "Forgotten", gone from the app; edit a memory in the app → used in the next
  chat; 300 memories → full.
