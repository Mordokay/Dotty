# Companion relay through the iPhone — design

After roles (0.3.0, released 2026-10-23) and the Bluetooth-relay spike (branch `spike/ble-relay`,
throwaway; results in CLAUDE.md). Next after this: scheduled moments (piece 3), then the iPhone link
(piece 4).

## Goal

When the iPhone is near, a chat with the companion goes through it over Bluetooth: no 5–8 s Wi-Fi
join and TLS handshake before the first reply, and it works with the iPhone locked in a pocket. Wi-Fi
stays as the fallback when the phone isn't there. Every companion feature works the same either way.

Measured in the spike: Bluetooth 23 KB/s each way, a command round trip of 58 ms; a relayed reply
starts 0.9–1.2 s after release, with the iPhone locked too; Gemini opened from the phone in 0.5 s. The
user: "way faster with Bluetooth"; ADPCM voice "clean enough".

## User's choices (2026-10-23)

- The phone is a relay with a key: Dotty keeps all the logic.
- Battery: Bluetooth only while Dotty is unlocked (plus a short grace window in a Wi-Fi chat, below):
  on while Dotty is unlocked (or, later, while a scheduled moment
  needs the internet), off while locked. iOS reconnects to a known Dotty by itself within 1–3 s of it
  advertising.
- At BOOT, if the phone isn't connected yet: start joining Wi-Fi **at once** while waiting for the
  phone; whichever path is ready first wins.
- If Wi-Fi wins, keep Bluetooth a little longer (a grace window) so the phone can still take over.
- Sound: IMA ADPCM, 4 bits a sample, is enough.

## Architecture

### Dotty keeps the logic

The instructions (character, memories, summaries, macros), the tools (memories, names, evolve, recall,
the writer, macros), the turn tracking, replay, history, usage counting and key rotation all stay on
Dotty, unchanged. The Gemini session (`GeminiLive`) gets a second **transport**: today it talks to
Gemini over its own WebSocket (Wi-Fi); with the relay it talks through the phone over Bluetooth. Both
carry the same Live API messages, so nothing above the transport changes.

- Outgoing: the JSON messages Dotty sends today (setup, `activityStart`/`activityEnd`, tool responses,
  `clientContent` turns) go to the phone as they are; the voice goes as ADPCM frames instead of
  base64 PCM.
- Incoming: the phone forwards Gemini's messages **without their audio** (the `inlineData` parts
  removed) and sends the audio separately as ADPCM frames, in the order it arrived. Dotty feeds both
  into the same message handling as the WebSocket (an audio frame counts as content of the turn it
  arrives in, for the turn tracker).

A bonus: while a chat goes through the relay, Bluetooth is on the whole time, so Dotty tells the app
(`companion.history`) after **every reply**, and the app's chat screen updates live instead of after the
chat (the user asked for that; with Wi-Fi chats it stays "after the chat").

### What the phone does

- Opens the Gemini Live WebSocket with the key Dotty names (by its last 4 characters; the key itself
  never leaves the Keychain or Dotty), sends Dotty's setup as it is, and forwards both ways.
- Converts audio: Dotty's voice ADPCM (16 kHz) → base64 PCM for Gemini; Gemini's PCM (24 kHz) →
  ADPCM for Dotty, in frames of 570 samples, each carrying its own start state.
- **Text-model calls for Dotty** (`relay.fetch`): in relay mode Dotty has no Wi-Fi, so the writer,
  recall and macros ask the phone, which POSTs the body to Google's `generateContent` for the named
  model with the same key and returns the status and body. Only that Google endpoint is allowed.
  The same call serves scheduled moments later.
- iOS: a relay service for the whole app (not a screen), attached at launch. Dotty's events wake it in
  the background (`bluetooth-central`); a background task per turn covers the reply, when Dotty sends
  nothing. One Gemini connection per chat: iOS drops an idle one about a minute after the chat (seen in
  the spike); the next press opens a new one (0.5 s).

## Choosing the path

- **Phone connected and relay-ready** (it sent `relay.hello`): the relay. Dotty doesn't touch Wi-Fi.
- **No phone yet** (just unlocked, or the phone is away): Dotty records as it does today, starts joining
  Wi-Fi in the background **and** keeps Bluetooth on. Whichever is ready first takes the chat, and the
  recording goes to the winner, so nothing said is lost:
  - the phone connects (and says hello) first → the Wi-Fi join is dropped; relay;
  - Wi-Fi and the Gemini session are ready first → Wi-Fi.
- **Grace window** (Wi-Fi won): Bluetooth stays on until the end of the first reply or 15 s, whichever
  is later. If the phone connects in that window, the next press moves the chat to the relay, passing
  the phone Google's session-resume handle so the same conversation continues, and Dotty's Wi-Fi goes
  off. Otherwise Bluetooth goes off for the rest of the chat, as today. (Bluetooth together with a Wi-Fi
  session is the tight-memory case: lows of 6–9 KB in the largest free block in the soak test, survived;
  the window keeps it short. Checked with the low-memory log during implementation.)
- The next chat races again, so the phone gets another chance every time Dotty is unlocked.

## Bluetooth protocol

- **Dotty → phone**, notifications on the Event characteristic:
  - JSON events (long ones in pieces, as today: every piece but the last starts with 0x1E):
    - `relay.open {setup, key, resume?}`: the whole setup message Dotty builds today, the key's last 4
      characters, and a resume handle when continuing a chat. Typically 5–15 KB (≈0.5 s); measured
      before any caching is added.
    - `relay.send {message}`: a Live API message as Dotty would send it.
    - `relay.fetch {id, model, body}` (a recall body can be up to 60 KB, ≈3 s).
    - `relay.close`.
  - Voice: raw notifications `0x1F 0x01 seq` + an ADPCM frame (the app sets 0x1F values aside).
- **Phone → Dotty**, writes without response on the Data characteristic (the phone waits for
  CoreBluetooth's `canSendWriteWithoutResponse` before each):
  - `0x01 seq` + an ADPCM frame: reply audio (24 kHz).
  - `0x02 flags` + bytes: a JSON message in pieces (flags bit 0 = more follows), reassembled on Dotty:
    Gemini's messages without audio, `{relay: "fetched", id, status, body}`, `{relay: "closed", code,
    reason}` (Gemini's own close: quota and errors reach Dotty's existing handling unchanged).
- **Handshake:** on connecting, the app sends the command `relay.hello {version, keys: [last 4 of each
  key in its Keychain]}`; Dotty relays only with a key the phone holds and only for a matching version.
- The fix found in the spike goes in too: `ble::notify` retries a short message like the pieces of a long
  one (a reply right after a burst was dropped).

## Failures

| Case | Behaviour |
|---|---|
| The phone drops mid-turn | that turn ends with "Lost the phone"; the next press chooses a path again |
| Gemini closes (quota, error) | the close reaches Dotty as from the WebSocket: key rotation, "out of words" as today |
| A fetch fails or times out (30 s) | the writer's / recall's existing failure path ("couldn't look back", "answer it yourself") |
| The phone has none of Dotty's keys | no relay: Wi-Fi |
| App in the background runs out of time | the turn may be cut; logged; the next press reopens |
| Old app (no `relay.hello`) | Wi-Fi, as today |

## Out of scope

Keeping Bluetooth on while Dotty is locked (battery: the user's call — it stays off); a per-reply live
chat for Wi-Fi chats (still after each chat); scheduled moments (piece 3, will reuse
`relay.fetch`); a different audio codec.

## Testing

- Host: the 0x02 piece reassembly; the relay transport's message conversion (audio stripped, frames
  in order, an audio frame counting as turn content); the path choice and grace window rules; the
  ADPCM codec (from the spike, with the C++ and Swift byte-for-byte pattern).
- Device: relay vs Wi-Fi; the race right after unlocking (phone first, Wi-Fi first); the grace window
  hand-over keeping the conversation; the phone dropping mid-turn; the iPhone locked in a pocket
  through the writer, recall, macros and the end-of-chat review; memory lows (the per-session log)
  during the grace window; the battery afterwards.
- App: build; the user's pass with the iPhone locked.
