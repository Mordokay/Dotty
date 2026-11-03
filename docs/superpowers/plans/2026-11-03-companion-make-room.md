# Companion "make room" Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Shrink the Companion image from 3.79 MB to ≤ 2.2 MB by moving the voice samples and 9 of the 10 avatars to the SD card (downloaded by the Companion itself) and leaving dev code out of release builds.

**Architecture:** The art tools write two packs (`cartridges/companion/assets/voices.bin`, `avatars.bin`) and a generated `assets_manifest.h` (name, size, SHA-256 each). `build_catalog.py` publishes the packs under fingerprinted names; on Dotty an `assets` module downloads missing or outdated packs over Wi-Fi in a background task while idle, verifies them, installs them into `data/`, and stamps them. Avatars are read from a PSRAM copy of `data/avatars.bin`, the robot (built in) standing in until then.

**Tech Stack:** Arduino-ESP32 3.x (pioarduino), C++17, Adafruit GFX, `net::download` (dotty_core), SD_MMC, Python 3 tools, `gh` CLI, host tests via `tools/companion_test/run.py` (clang++).

**Spec:** `docs/superpowers/specs/2026-11-03-companion-make-room-design.md`

## Global Constraints

- Pack names on releases and the bench server: `companion-<stem>-<sha8>.bin` (`<sha8>` = first 8 hex digits of the pack's SHA-256), e.g. `companion-avatars-1a2b3c4d.bin`.
- Release URL: `https://github.com/Mordokay/Dotty/releases/latest/download/<fingerprinted name>`.
- Card paths (Companion's own data dir, `storage::myDataDir()`): `data/avatars.bin`, `data/voices/<name>.mp3`, `data/assets/<stem>.stamp` (the installed pack's full SHA-256, 64 hex), downloads via `data/assets/<stem>.part`.
- Avatar pack format `DAV1`: magic, u32 count, per avatar `id[16]` (zero-padded), then per avatar in that order 12 × 2048 B (128 px) then 12 × 288 B (48 px); rows MSB-first, set bit = black. The robot is not in it.
- Retry schedule: first try 30 s after start, then 10 min after a failure, then every 60 min.
- Download only while idle: no session open or connecting, no trip, no voice note, no Wi-Fi transfer, no line recording/writer job. Anything that needs Wi-Fi or a session cancels it.
- Bench-only code inside `#if !defined(DOTTY_RELEASE)`; `cartridges/companion/bench.h` (gitignored) may define `COMPANION_ASSET_BASE` (a URL ending in `/`), used only when `!DOTTY_RELEASE`.
- dotty_core's serial keys (`1`-`9`, `[ ] { }`, `s`, `d`, `k`, `B`, `p`) stay in every build.
- Logs via `LOGI/LOGW/LOGE`, never `Serial.print`. Large buffers in PSRAM (`ps_malloc`).
- `firmware.bin` for `companion` ≤ 2.2 MB (2,306,867 bytes) after Task 6.
- Every commit ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; before any push the key checks print 0: `git log -p FETCH_HEAD..HEAD | grep -cE "AIza[0-9A-Za-z_-]{35}|AQ\.[0-9A-Za-z_.-]{40,}"` and `git ls-files | grep -c 'dev_key.h\|bench.h'`.

## Review Focus

- **A pack that's on the card but from an older firmware** (stamp SHA ≠ manifest): must be treated as missing and re-downloaded, while the old avatars keep drawing until the new file is verified (never a half-written `avatars.bin`). Test: Task 3 `ready()` with a stale stamp; Task 4 installs via `.part` + rename.
- **Card removed or full mid-download**: the download fails, `.part` is deleted, the Companion keeps working with the robot / old files, and retries on schedule. Test: Task 4 device step (pull the card is not possible at the desk; use a bench server that closes mid-file).
- **A companion set to an avatar id the pack lacks** (unknown id, or `photo` with no photo): draws the robot, never crashes on a null face. Test: Task 1 `AvatarPack::face("nope", …) == nullptr` + `avatar::draw` fallback.
- **A session (BOOT press) starting while a download runs**: the download stops at once (Wi-Fi is the chat's), the chat isn't delayed by more than a moment. Test: Task 4 device step, press `r` during a download.
- **Truncated or wrong-magic `avatars.bin`** (power cut before rename can't happen, but a hand-copied file can): rejected at load, robot shown, file re-downloaded. Test: Task 1 short-file and bad-magic cases.

---

### Task 1: Avatar pack (robot built in, the rest from a pack)

**Files:**
- Create: `cartridges/companion/avatar_pack.h` (header-only, plain C++)
- Create: `tools/companion_assets.py` (manifest + fingerprint helpers)
- Create: `cartridges/companion/assets/avatars.bin` (generated)
- Create: `cartridges/companion/assets_manifest.h` (generated)
- Modify: `tools/avatar_art.py` (`main`, `OUT`)
- Modify: `cartridges/companion/images/avatars.h` (regenerated: robot only)
- Modify: `cartridges/companion/avatar.h`, `cartridges/companion/avatar.cpp`
- Test: `tools/companion_test/test_avatar_pack.cpp`

**Interfaces:**
- Produces: `class AvatarPack { AvatarPack(const uint8_t *data, size_t size); bool valid() const; int count() const; const char *id(int i) const; const uint8_t *face(const char *id, int face, bool big) const; static constexpr size_t kBigFace = 2048, kSmallFace = 288; static constexpr int kFaces = 12; };` — `face` returns nullptr when invalid, unknown id or face out of range.
- Produces: `avatar::setPack(const uint8_t *data, size_t size)` (nullptr clears; the caller owns the buffer), `bool avatar::packed(const String &id)` (true for robot, photo, or an id the current pack has).
- Produces: `tools/companion_assets.py`: `ASSETS = REPO/"cartridges/companion/assets"`, `fingerprint_name(path) -> str` (`companion-<stem>-<sha8>.bin`), `write_manifest()` → `cartridges/companion/assets_manifest.h` with `struct AssetPack { const char *stem; const char *file; uint32_t size; const char *sha256; };` and `constexpr AssetPack kAssetPacks[] = {...}` (sorted by stem; `file` = fingerprinted name), `check_manifest()` (exit non-zero if the header disagrees with the files). CLI: `python3 tools/companion_assets.py [--check]`.
- `images/avatars.h` keeps `kAvatarCount = 10`, `kAvatarIds`, `kAvatarNames` (all 10, same order) and gets `kRobotBig[kFaceCount][2048]`, `kRobotSmall[kFaceCount][288]` instead of `kBig`/`kSmall`.

- [ ] **Step 1: Write the failing test** `test_avatar_pack.cpp` building a 2-avatar `DAV1` buffer in memory (ids "dog", "cat"; each face's first byte = avatar index × 16 + face) and checking: `valid()`; `count() == 2`; `id(1) == "cat"`; `face("cat", 3, true)[0] == 0x13`; offsets follow the format (header `8 + 16·n`, then per avatar a block of `12·2048 + 12·288 = 28,032` bytes, big faces first): `face("cat", 3, true) - buf == 8 + 32 + 28032 + 3·2048` and `face("cat", 3, false) - buf == 8 + 32 + 28032 + 24576 + 3·288`; `face("robot", 0, true) == nullptr`; `face("dog", 12, true) == nullptr`; a buffer one byte short → `!valid()` and every `face` nullptr; magic `"DAV2"` → `!valid()`.
- [ ] **Step 2: Run** `python3 tools/companion_test/run.py avatar_pack` — expected: BUILD FAILED (no `avatar_pack.h`).
- [ ] **Step 3: Implement `AvatarPack`** in `avatar_pack.h`, in `voice_pack.h`'s style (little-endian u32 count at 4, ids at 8 + 16·i, faces from 8 + 16·count; `valid()` requires exact size `8 + 16n + n·12·(2048+288)` and `n ≤ 32`).
- [ ] **Step 4: Run** the test — expected: PASS.
- [ ] **Step 5: Change `tools/avatar_art.py`** to write the robot-only header (`kRobotBig`, `kRobotSmall`) and `assets/avatars.bin` (the other 9, `IDS` order minus robot), then call `companion_assets.write_manifest()`. Write `tools/companion_assets.py`. Run `python3 tools/avatar_art.py` — expected: `assets/avatars.bin` is `8 + 9·16 + 9·12·2336 = 252,440` bytes; `python3 tools/companion_assets.py --check` exits 0.
- [ ] **Step 6: `avatar.cpp`**: `draw` takes robot faces from `kRobotBig/kRobotSmall`, other ids from the pack set by `setPack` (via `AvatarPack::face`), the robot when that's nullptr. `count/id/name/known` unchanged (all 10 ids).
- [ ] **Step 7: Build** `~/.platformio/penv/bin/pio run -e companion` — expected: SUCCESS; `nm --size-sort -S` shows no `art::kBig`, and `firmware.bin` ~270 KB smaller than 3,886,512.
- [ ] **Step 8: Commit** `git add tools/avatar_art.py tools/companion_assets.py cartridges/companion/{avatar_pack.h,avatar.h,avatar.cpp,images/avatars.h,assets/avatars.bin,assets_manifest.h} tools/companion_test/test_avatar_pack.cpp` — "Companion: avatars except the robot in a pack for the card".

### Task 2: Voice samples out of the image

**Files:**
- Move: `cartridges/companion/voices.bin` → `cartridges/companion/assets/voices.bin` (`git mv`)
- Modify: `tools/build_voice_pack.py` (`OUT`; call `companion_assets.write_manifest()`)
- Modify: `platformio.ini` (`[env:companion]`: drop `board_build.embed_files`)
- Modify: `cartridges/companion/main.cpp` (`playSample`, remove the `voicePackStart/End` externs)
- Test: `tools/companion_test/test_voice_pack.cpp`

**Interfaces:**
- Consumes: `companion_assets.write_manifest()` (Task 1).
- Produces: `playSample(const char *name)` plays `data/voices/<name>.mp3` when the file exists; otherwise `showNote("Not downloaded yet")`. No flash reads.

- [ ] **Step 1: Add a test** to `test_voice_pack.cpp`: `voice pack file has 30 samples` — read `../../cartridges/companion/assets/voices.bin` (the runner's cwd is `tools/companion_test`), `VoicePack(...).valid()`, `count() == 30`, `find("Zephyr", …)` true with `len > 0`.
- [ ] **Step 2: Run** `python3 tools/companion_test/run.py voice_pack` — expected: FAIL (file not found).
- [ ] **Step 3:** `git mv` the pack, point `build_voice_pack.py` at `assets/voices.bin` + manifest, drop the embed line, rewrite `playSample` as in Interfaces (keep its busy guard).
- [ ] **Step 4: Run** the test (PASS), `python3 tools/companion_assets.py --check` (exit 0, manifest has `avatars` and `voices`), and `pio run -e companion` — expected: SUCCESS, `firmware.bin` about 1.4 MB smaller again.
- [ ] **Step 5: Commit** — "Companion: voice samples no longer in the image".

### Task 3: Asset rules (host-tested)

**Files:**
- Create: `cartridges/companion/asset_plan.h` (header-only, plain C++)
- Test: `tools/companion_test/test_asset_plan.cpp`

**Interfaces:**
- Consumes: `AssetPack` from `assets_manifest.h` (Task 1).
- Produces: `bool assetReady(const std::string &stampText, const AssetPack &p)` (stamp, whitespace-trimmed, equals `p.sha256`); `std::string assetUrl(const std::string &base, const AssetPack &p)` (`base + p.file`); `class AssetRetry { uint32_t due() const; void started(uint32_t nowMs); void failed(uint32_t nowMs); void succeeded(); bool isDue(uint32_t nowMs) const; }` — after construction due at 30,000 ms; first failure → +600,000; later failures → +3,600,000; `succeeded()` → never due until `reset(uint32_t nowMs)` (due 30 s later; used when a stamp turns stale).
- Produces: `constexpr const char *kReleaseAssetBase = "https://github.com/Mordokay/Dotty/releases/latest/download/";`

- [ ] **Step 1: Write failing tests**: `assetReady` true for the exact sha and for sha + "\n", false for "", for another sha, for a 63-char prefix; `assetUrl(kReleaseAssetBase, p)` ends with `p.file`; `AssetRetry`: `isDue(29999)` false, `isDue(30000)` true; after `failed(40000)` → `due() == 640000`; after a second `failed(700000)` → `due() == 4300000`; `succeeded()` → `isDue(UINT32_MAX)` false; `reset(5000)` → `due() == 35000`.
- [ ] **Step 2: Run** `python3 tools/companion_test/run.py asset_plan` — expected: BUILD FAILED.
- [ ] **Step 3: Implement** `asset_plan.h`. (millis wrap-around is ignored: Dotty restarts long before 49 days matter here, as elsewhere in the cartridge.)
- [ ] **Step 4: Run** — expected: PASS; then the whole suite `python3 tools/companion_test/run.py` — all pass.
- [ ] **Step 5: Commit** — "Companion: asset readiness and retry rules".

### Task 4: Downloading and installing the packs on Dotty

**Files:**
- Create: `cartridges/companion/assets.h`, `cartridges/companion/assets.cpp`
- Modify: `cartridges/companion/main.cpp` (setup, loop, the idle rule, cancel points, `companion.status`, settings voice row)
- Modify: `.gitignore` (`cartridges/companion/bench.h`)

**Interfaces:**
- Consumes: `AvatarPack`, `avatar::setPack` (Task 1), `assetReady`, `assetUrl`, `AssetRetry`, `kReleaseAssetBase` (Task 3), `kAssetPacks` (Task 1), `net::connect/download/disconnect`, `storage::myDataDir()`, `power::setWakeLock`.
- Produces (namespace `assets`):
  - `void begin()` — in `setup()` after storage: reads stamps; if the avatars stamp is ready and `data/avatars.bin` loads (`ps_malloc`, `AvatarPack::valid()`), calls `avatar::setPack`; logs `assets: avatars ready` / `assets: voices ready` / `… missing`.
  - `void poll(bool idle)` — every loop pass (locked branch too): when some pack isn't ready, `idle` and its `AssetRetry` is due, starts the download task (core 0, 12 KB stack, `kWakeLockNetwork` held while it runs); when the task has finished, on the main loop: installs avatars (`setPack` with the new buffer, frees the old one) and sets the redraw flag that `takeChanged()` returns.
  - `void cancel(const char *why)` — sets the task's stop flag; returns at once; the task drops `.part`, disconnects Wi-Fi only if it connected it, and counts as a failure (`AssetRetry::failed`).
  - `bool busy()`, `bool takeChanged()`, `const char *state(const char *stem)` → `"ready" | "downloading" | "missing"`, `bool voicesReady()`.
- The task per pack: `net::connect` → `net::download(assetUrl(base, p))` into `data/assets/<stem>.part` (sink returns false when stopped or the card write fails) → size and SHA-256 (mbedtls) must equal the manifest → install: `avatars`: remove `data/avatars.bin`, rename the `.part` onto it, load it into a new PSRAM buffer for the main loop to take; `voices`: read the `.part` into PSRAM, write each sample to `data/voices/<name>.mp3` via `<name>.mp3.part` + rename, delete the pack `.part` → only then write `data/assets/<stem>.stamp`. Logs `assets: downloading <stem> (<size> bytes)`, `assets: <stem> done in <s> s`, `assets: <stem> failed: <reason>`.
- `base` = `COMPANION_ASSET_BASE` when `!defined(DOTTY_RELEASE) && __has_include("bench.h")` defines it, else `kReleaseAssetBase`.
- main.cpp: `bool assetsIdle()` = no session open or connecting, no relay chat, `!trip.active`, `!note::active()`, `!transfer::active()`, `!lineJob.busy()`, no writer/recall job, no phone wake running. Call `assets::cancel("…")` wherever a session starts connecting, a phone wake begins, a transfer starts, a trip starts. On `assets::takeChanged()` set `redraw = true`. `companion.status` gets `"assets": {"avatars": state("avatars"), "voices": state("voices")}`. In the settings voice row, `playSample` already shows "Not downloaded yet" (Task 2).

- [ ] **Step 1: Write `assets.h/.cpp`** per the Interfaces, and wire `main.cpp`.
- [ ] **Step 2: Build** `pio run -e companion` — expected: SUCCESS.
- [ ] **Step 3: Bench server for testing** — until Task 5's `serve_assets.py` exists, serve by hand: `mkdir -p $SCRATCH/assets && for f in avatars voices; do cp cartridges/companion/assets/$f.bin $SCRATCH/assets/$(python3 tools/companion_assets.py --name $f); done; python3 -m http.server 8000 -d $SCRATCH/assets` (add `--name STEM` printing the fingerprinted name to `companion_assets.py`), and `bench.h` with `#define COMPANION_ASSET_BASE "http://<Mac's IP>:8000/"`.
- [ ] **Step 4: Device test, fresh card state** — add dev key `$` (inside `!DOTTY_RELEASE`, kept): deletes `data/avatars.bin`, `data/voices/`, `data/assets/` and restarts. Flash, set a non-robot companion active, press `$`. Expected in the log after reboot: `assets: avatars missing`, `assets: voices missing`, at ~30 s `downloading avatars (252440 bytes)`, `done in N s`, `downloading voices (1407752 bytes)`, `done`; screenshot (`tools/screenshot.py`) shows the robot before and the real avatar after; settings' voice row plays a sample.
- [ ] **Step 5: Device test, second boot** — reset: the log says both ready, no download.
- [ ] **Step 6: Device test, interruptions** — during a download press `r` (simulated BOOT): log `assets: … failed: cancelled (a chat)`, the chat connects normally, and the log says `assets: next try in 600 s`. Stop the bench server mid-voices: failure logged, no `.part` left (check with a dev listing or the next successful run), old files untouched.
- [ ] **Step 7: Commit** (never `bench.h`) — "Companion: downloads its voices and avatars to the card when idle".

### Task 5: Publishing the packs

**Files:**
- Modify: `tools/build_catalog.py` (the `--release` file list; manifest check)
- Create: `tools/serve_assets.py`
- Modify: `tools/companion_assets.py` (`--name STEM` if not already added in Task 4)

**Interfaces:**
- Consumes: `companion_assets.check_manifest()`, `fingerprint_name(path)`.
- Produces: release files include every `cartridges/*/assets/*.bin`, copied into `dist/` under `fingerprint_name`; `build_catalog.py` exits before building if `check_manifest()` fails ("assets_manifest.h is stale: run tools/avatar_art.py / tools/build_voice_pack.py"). `tools/serve_assets.py [--port 8000]`: copies the packs to fingerprinted names in a temp folder, prints the `COMPANION_ASSET_BASE` line for `bench.h` with the Mac's IP, serves until Ctrl-C.

- [ ] **Step 1:** Implement both; run `.venv/bin/python tools/build_catalog.py` (no `--release`) — expected: `dist/` holds `companion-avatars-<sha8>.bin` and `companion-voices-<sha8>.bin` with the manifest's sizes.
- [ ] **Step 2:** Append one byte to `assets/avatars.bin`, run `build_catalog.py` → expected: exits with the stale-manifest message before building; then `git checkout cartridges/companion/assets/avatars.bin`.
- [ ] **Step 3:** `python3 tools/serve_assets.py` and repeat Task 4 Step 4's fresh download against it — expected: same log.
- [ ] **Step 4: Commit** — "Catalog: publishes the Companion's packs; serve_assets.py for the bench".

### Task 6: Dev code out of release builds, one copy of the big font

**Files:**
- Modify: `cartridges/companion/main.cpp` (the "Developer aids." block in the input handler, the helpers only it calls — `fakeTrip`, `textModelTest`, test photo/name/note helpers, the location probe — and the dev-only flags)
- Create: `lib/dotty_core/src/fonts.h`, `lib/dotty_core/src/fonts.cpp`
- Modify: `lib/dotty_core/src/shell.cpp`, `lib/dotty_core/src/lock_screen.cpp`, `cartridges/companion/screens.cpp`, `cartridges/companion/nav_screen.cpp` (use `fonts.h` instead of `<Fonts/FreeSansBold24pt7b.h>`)

**Interfaces:**
- Produces: `fonts.h`: `extern const GFXfont &kBold24;` defined in `fonts.cpp` (the only file including `<Fonts/FreeSansBold24pt7b.h>` among these four); callers use `&kBold24` where they passed `&FreeSansBold24pt7b`.
- Dev-only flags (`simulatedHold`, `keepBle`, `navFake`, `forcePlan`, `noteTestUntil`, …) become `constexpr bool … = false` / `constexpr uint32_t … = 0` in release builds so the code reading them stays as is.

- [ ] **Step 1:** Wrap the Companion's dev keys and their helpers in `#if !defined(DOTTY_RELEASE)`; add `fonts.h/.cpp` and switch the four files.
- [ ] **Step 2: Build both ways** — `pio run -e companion` (USB) and `PLATFORMIO_BUILD_FLAGS=-DDOTTY_RELEASE pio run -e companion` — both SUCCESS; on the release ELF `nm -C … | grep -c "fakeTrip\|textModelTest"` → 0 and `grep -c FreeSansBold24pt7bBitmaps` → 1; release `firmware.bin` ≤ 2,306,867 bytes. Also `pio run -e launcher` and `-e music` build (dotty_core change).
- [ ] **Step 3: Device check (USB build)** — `>` still starts a fake walk; lock screen clock and directions' big numbers render as before (screenshot).
- [ ] **Step 4: Commit** — "Companion: dev keys only in USB builds; the big bold font compiled once".

### Task 7: Docs, versions, release, and the real download

**Files:**
- Modify: `CLAUDE.md` (Companion section: assets bullet — packs, card paths, manifest, retry, bench server, `$` dev key; the size; `DOTTY_RELEASE` dev code; the `voices.bin` mention under "Screens"; Avatars bullet), memory `dotty-ai-companion.md` roadmap line (make room done).
- Modify: every catalog cartridge's `DOTTY_CARTRIDGE` version (dotty_core changed): companion → `0.9.0`, the others +1 patch.

- [ ] **Step 1:** Update the docs and versions; `python3 tools/companion_test/run.py` — all pass.
- [ ] **Step 2:** Commit, run the key checks (Global Constraints), push (`H='credential.helper=!gh auth git-credential'; git -c "$H" push -q https://github.com/Mordokay/Dotty.git main`, then `git update-ref refs/remotes/origin/main HEAD`), `.venv/bin/python tools/build_catalog.py --release` — expected: the release lists both packs; `gh release view <tag> --json assets` shows them.
- [ ] **Step 3: The real download** — flash the USB build with no `bench.h` base (rename it aside), wipe the packs with `$`, reboot: the log shows both downloaded from `github.com` and the avatar appears. Restore `bench.h` after.
