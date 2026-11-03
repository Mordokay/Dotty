# Companion: make room — design

Date: 2026-11-03. Status: approved in chat, spec for review.

## Why

The Companion image is 3.79 MB of its 4 MB `ota_0` slot (93 %), so every new feature is close to the
limit. Growing `ota_0` into the unused 1.1 MB `storage` partition stays parked (it needs a USB
re-flash of the partition table). Instead the big assets leave the image for the SD card, and dev-only
code leaves release builds.

Measured (`nm --size-sort`, `firmware.bin` 3,886,512 bytes):

| What | Size | Used for |
|---|---|---|
| `voices.bin` (embedded) | 1,407,752 B | 30 voice samples, played when flipping voices in Dotty's settings |
| `art::kBig` + `art::kSmall` | 245,760 + 34,560 B | 10 avatars × 12 faces at 128 px and 48 px |
| `FreeSansBold24pt7b` | 4 × 8,143 B | the same font compiled into four files |
| dev keys + helpers | tens of KB | ~50 serial test keys, `fakeTrip`, `textModelTest`, test photo… |

## Goals

- The image near 2.1 MB; the app and Dotty's screens behave as today once the files are on the card.
- Dotty works without the iPhone, including the very first start (no blank or missing avatar).
- The sandbox rule holds: the Companion only writes its own `data/` folder.
- Release (catalog) builds carry no dev code; USB builds keep it.

Not goals: the partition change, app changes, other cartridges.

## Decisions (with the user, 2026-11-03)

- **The Companion downloads its own files** over Wi-Fi from the latest GitHub release (not the launcher,
  not the iPhone).
- **The robot stays built in** (its 24 faces, ~27 KB) as the stand-in for any avatar whose file isn't
  on the card yet; voices before the download say "Not downloaded yet".
- **Dev code behind `DOTTY_RELEASE`** (defined by `tools/build_catalog.py` since 2026-11-03): the
  Companion's own dev keys and their helpers go; dotty_core's serial keys (taps `1`-`9`, swipes,
  screenshot `s`, log replay `d`, lock `k`, battery `B`) stay in every build.

## 1. What leaves the image

**Voices.** `tools/build_voice_pack.py` keeps making the same `DVS1` pack, now written to
`cartridges/companion/assets/voices.bin` (no longer embedded; `board_build.embed_files` goes). On the card
it's unpacked once into `data/voices/<name>.mp3` (today's per-sample copies already live there), then
the pack itself is deleted. Playing a sample = `player.play` of that file, as today.

**Avatars.** `tools/avatar_art.py` writes:
- `images/avatars.h`: the ids and names of all 10 (the app's order, unchanged) and the **robot's** 12 + 12
  faces only;
- `assets/avatars.bin`: the other 9 avatars, format `DAV1` + count + per avatar {id[16]} then the faces
  back to back, in id order: per avatar 12 × 2048 B (128 px) then 12 × 288 B (48 px), rows MSB-first, set
  bit = black (as now). ~247 KB.

At start the Companion reads `data/avatars.bin` into one PSRAM buffer (checked against the expected size
and magic) and `avatar::draw` takes a face from it; an avatar that isn't there (no file yet, or a bad one)
draws as the robot. A file that arrives later is loaded and the screen redrawn.

**Font.** `FreeSansBold24pt7b` comes into one file only and the others use it through a declaration.

**Dev code.** Everything in `onKey`'s Companion section and the helpers only it calls go inside
`#if !defined(DOTTY_RELEASE)`; so do the dev-only flags (`simulatedHold`, `keepBle`, `navFake`,
`forcePlan`, `noteTestUntil`…) where the release path can treat them as false. The bench key
(`dev_key.h`) already follows this rule.

## 2. Getting the files

**Fingerprints.** Each pack's name carries its SHA-256's first 8 hex digits:
`companion-voices-<sha8>.bin`, `companion-avatars-<sha8>.bin`. The art tools also write a generated
`assets_manifest.h` with, per pack, the file name, size and full SHA-256, so a firmware knows exactly
which packs it wants.

**Publishing.** `build_catalog.py` uploads every `cartridges/<id>/assets/*.bin` to the release under its
fingerprinted name (a few MB per release). The latest release always holds the packs the latest
Companion wants; the URL is
`https://github.com/Mordokay/Dotty/releases/latest/download/<fingerprinted name>`. The packs stay in git
(as `voices.bin` already is) so any build can be reproduced.

**On Dotty** (`assets.*`, a small module with a host-tested core):
- On the card: `data/assets/<pack>.stamp` holds the SHA-256 of the pack last installed. A pack is
  **ready** when its stamp matches the manifest (and, for avatars, the file loads with the right size).
  No re-hashing of 1.4 MB at every start.
- When one isn't ready, a background task (core 0, 12 KB stack, Network wake lock) joins Wi-Fi
  (`net::connect`), downloads to `<name>.part` (`net::download`), checks size and SHA-256, then installs
  it: avatars → rename to `data/avatars.bin`; voices → unpack into `data/voices/`, delete the pack. Only
  then is the stamp written. Wi-Fi goes off after.
- When: only while Dotty is idle — no chat or session connecting, no trip, no voice note, no Wi-Fi
  transfer, no line recording; first ~30 s after start, then after 10 min, then every hour (also at
  locked minute wakes when due). Anything that needs Wi-Fi or a session cancels it (the `.part` is
  dropped, tried again later).
- Logs: `assets: avatars ready`, `assets: downloading voices (1407752 bytes)`, `… done in N s`,
  failures with the reason.

**Bench builds.** A gitignored `cartridges/companion/bench.h` may define `COMPANION_ASSET_BASE` (e.g.
`"http://192.168.68.50:8000/"`) used instead of the release URL, for art not released yet:
`tools/serve_assets.py` copies `cartridges/companion/assets/*.bin` to their fingerprinted names in a
scratch folder and serves it over HTTP. Only in USB builds (`!DOTTY_RELEASE`), like `dev_key.h`.
(The art tools write the plain names, `assets/voices.bin` and `assets/avatars.bin`, plus the manifest;
the fingerprinted names exist only on release uploads and the bench server.)

## 3. What the user sees

- First start with no Wi-Fi: everything works; a non-robot avatar shows as the robot; settings' voice
  row plays nothing and the hint says "Not downloaded yet". Once Wi-Fi is reachable the files arrive
  and the next redraw shows the real avatar.
- `companion.status` gets `assets: {avatars, voices}` (each `ready` | `downloading` | `missing`) for the
  app or the Mac tool to read; the app doesn't change.
- A removed card or a failed download never blocks the Companion.

## Testing

- Host tests (`tools/companion_test/run.py`): the `DAV1` reader (good file, short file, wrong magic,
  unknown id → robot), the stamp/manifest readiness rule, the retry schedule, the voice pack unpacking
  plan (names, sizes) on a pack made by `build_voice_pack.py`.
- Device: delete `data/avatars.bin`, `data/voices/` and the stamps; boot with a non-robot companion →
  robot shown, the log shows both downloads, the avatar appears; voices play. Boot again → no
  download. A wrong SHA-256 (corrupt `.part` via the bench server) → refused, retried.
- Sizes: `firmware.bin` ≤ 2.2 MB; a `DOTTY_RELEASE` build has no dev key handlers (e.g. the `fakeTrip`
  symbol is absent) and passes `check_no_secrets`.
- Release: the catalog release has both packs, and a Dotty updated from it downloads them.

## Changes during the build (2026-11-03)

- **No bench server** (the user doesn't want a local server for this project): `bench.h`,
  `COMPANION_ASSET_BASE` and `serve_assets.py` were dropped; packs only come from the latest release.
- **Downloads only with Bluetooth off** (locked, as a rule): with the iPhone connected, joining Wi-Fi left
  13 KB of internal RAM and HTTPS failed or aborted the system. A RAM floor (40 KB free, 20 KB block) guards
  HTTPS, and dotty_core's `shell::Config::beforeBluetooth` ends a download before the shell turns Bluetooth
  on (unlocking mid-download had frozen NimBLE: interrupt watchdog). First start therefore shows the robot
  until Dotty first locks.
