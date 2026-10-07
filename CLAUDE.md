# CLAUDE.md

Dotty is an evolving desk-companion firmware project: features are decided iteratively
with the user. Read `README.md` for the overview and `docs/HARDWARE.md` for the board.

## Hardware facts (verified on the device)

- Board is the **Waveshare ESP32-S3-Touch-ePaper-1.54 V2** — e-paper, not an LCD.
  An earlier handover from another model described a different board (240×240 LCD with
  QMI8658 IMU and ES7210); none of that applies.
- `esptool flash-id`: ESP32-S3 rev 2, **8 MB flash (GD, quad), 8 MB OPI PSRAM**.
- **BLE only** — no Bluetooth Classic, so no A2DP audio streaming. For phone media
  control use Apple Media Service (AMS); for iOS notifications, ANCS.
- **No IMU** (no shake detection). The SHTC3 temperature/humidity sensor sits on the board
  and reads the board's heat (+8 °C measured), not the room's.
- e-paper timing measured: full refresh ≈ 1.9 s (flashes), partial ≈ 0.6 s. Design UI
  for slow, discrete frame changes — not smooth animation.
- Power: on battery the board stays on only while **GPIO17 is high** (soft latch). Set it
  first thing in `setup()`. GPIO6 LOW = e-paper power on, GPIO42 LOW = audio power on.
- **GPIO42 switches the audio rail (ES8311, mic, amp). While it is off, the unpowered
  ES8311 clamps the shared I2C bus**, so touch, RTC and SHTC3 all NACK. Turn the rail on
  before any I2C access (the power-saving code toggles it around each sleep).
  Touch, RTC, SHTC3, SD card and I2C pull-ups are all on the always-on 3V3.
  The FT6336 NACKs its ID register 0xA8; probe it via 0x02 (touch status).
- Schematic: `04_Hardware/Schematics/` in the Waveshare repo (render the PDF with
  `qlmanage -t -s 4000` and crop with `sips`). Charger STAT only drives the orange LED,
  so firmware cannot detect charging. No 32 kHz crystal: the ESP32 sleep clock drifts,
  so re-read the PCF85063 after every wake instead of trusting system time.
- Touch coordinates map 1:1 onto the display (no rotation/mirroring).
- Audio pins (from Waveshare's codec_board config): I2S MCLK 14, BCLK 15, WS 38,
  DOUT 45, DIN 16; amplifier enable GPIO46 (HIGH). ES8311 at I2C 0x18.
- Microphone: analog, into the ES8311 ADC → I2S DIN, left slot of the stereo frames.
  `Es8311::setMicrophone(on, gainDb)` = reg 0x0A bit 6 (ADC port mute) + reg 0x16 (PGA,
  0..7 = 0..42 dB; 30 dB like Waveshare's default) + reg 0x17 (ADC digital volume, 0xBF =
  0 dB, 0.5 dB steps). Tape uses 30 dB + 2.5 dB digital: a normal voice was a bit low. Measured with a 0.5-16 kHz sweep from
  the Mac speakers: strong response up to ~13.5 kHz, rolling off near 16 kHz; quiet room
  ≈ -60 dBFS. So 32 kHz sampling is worth it (16 kHz would cut the 8-16 kHz "s" sounds).
- Buttons are active-low with pull-ups. The PWR button is still held down right after a
  battery power-on, so ignore it until it has been released once.
- Battery: `analogReadMilliVolts(4) * 2`.
- RTC PCF85063 at I2C 0x51 holds local time. Set by the phone on every connection
  (`core.time`) and by Weather's SNTP; at boot firmware sets it to the build time
  (`__DATE__`/`__TIME__`) when it is invalid or older.
- SD card is **SDMMC 1-bit** (CLK 39, CMD 41, D0 40), not SPI.

## Toolchain decisions

- **VS Code + pioarduino IDE** (community fork of PlatformIO). The official PlatformIO
  espressif32 platform lags on Arduino-ESP32 3.x; Waveshare requires core ≥ 3.3.0.
  `platformio.ini` pins the pioarduino `stable` platform and documents every Arduino IDE
  Tools-menu option it replaces — keep those comments when changing settings.
- `pio` path: `~/.platformio/penv/bin/pio` (also symlinked in `~/.local/bin`).
- Known installer bug: pioarduino left a `uv` *binary* at `~/.platformio/.cache/uv`,
  which blocks uv's cache dir ("Failed to initialize cache … File exists"). Fix: move the
  file aside.

## Commands

```bash
~/.platformio/penv/bin/pio run -e music                                # build a cartridge
~/.platformio/penv/bin/pio run -e launcher -t upload --upload-port /dev/cu.usbmodem1101  # ota_1, boots launcher
~/.platformio/penv/bin/pio run -e rescue -t upload --upload-port /dev/cu.usbmodem1101    # factory (Rescue), boot slot unchanged
~/.platformio/penv/bin/pio run -e music -t upload --upload-port /dev/cu.usbmodem1101     # ota_0, boots music
~/.platformio/penv/bin/esptool --port /dev/cu.usbmodem1101 flash-id    # chip info
```

- **Never use `Serial.print` directly — use `LOGI/LOGW/LOGE(tag, fmt, ...)` from
  `lib/dotty_core/src/log.h`.** With the cable plugged in but no monitor reading, `Serial.print` blocks
  up to 2 s once the 256-byte USB TX buffer fills (this made taps lag 2-3 s).
  The logger writes to a 16 KB RAM ring and streams it only when there is room.
- Lines streamed while no monitor is open are lost by macOS, but the ring keeps the last
  16 KB: send `d` over serial to replay the history. To read logs non-interactively,
  open the port with pyserial from `~/.platformio/penv/bin/python`, write `d`, read
  lines (no reset needed). To catch boot from scratch, pulse RTS to reset instead.
- Don't call `Serial.setTxTimeoutMs(0)`: it stopped all USB serial output.
- **Loop watchdog + crash dumps** (cartridges, not the launcher): a loop pass that doesn't
  return for 90 s trips the task watchdog → panic → core dump to the `coredump` partition →
  restart. The next boot logs `the last run crashed (reset reason N)` and a summary (the
  running task, usually IDLE — not the stuck one). Full dump with every task's backtrace:
  `esptool --port P read-flash 0x7F0000 0x10000 dump.bin`, then `.venv/bin/esp-coredump
  info_corefile --gdb ~/.platformio/packages/tool-xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb
  -t raw -c dump.bin .pio/build/<env>/firmware.elf` (needs the ELF of that exact build).
  Verified with a deliberate hang (loopTask shown at the hanging line). A live stuck
  Dotty can also be inspected without restarting: OpenOCD (`tool-openocd-esp32`,
  `board/esp32s3-builtin.cfg`) + `xtensa-esp32s3-elf-gdb`, `thread apply all bt`.
- **The USB serial port must stay quiet without a computer**: locking after unplugging
  crashed with an interrupt-watchdog reset on CPU1 (an interrupt storm; the log task was in
  its serial code). `log.cpp` only streams while `HWCDC::isPlugged()`; library log levels
  are errors only (`CORE_DEBUG_LEVEL=1`, `CONFIG_NIMBLE_CPP_LOG_LEVEL=1`). Don't add
  `Serial.print`s or raise those levels for releases.
- PWR is debounced (30 ms) and ignored for 600 ms after a lock/unlock; a light-sleep wake
  unlocks only if PWR is really down (else "woke for PWR, but it isn't pressed"). Locking
  on battery once flashed the lock screen and bounced straight back to the app.
- **Taps without a finger**: serial keys `1`-`9` tap a 3x3 grid like a phone keypad (`1` =
  the nav bar's left corner, `3` its right corner, `5` the middle); `]` / `[` swipe left / right, `}` / `{` swipe up / down;
  `k` locks / unlocks (while locked it's read only when Dotty is awake: a locked Dotty without
  a reading program light-sleeps until the next minute). Send keys with a short
  wait before closing the port (`write; flush; sleep 0.3`): closing at once left the byte
  queued in macOS until the next open, where the screenshot's `s` overwrote it.
- **Screenshots without a camera**: send `s` over serial (only while a computer has the
  port) → Dotty prints `#SCREEN 200 200`, the framebuffer as hex lines, `#END`;
  `.venv/bin/python tools/screenshot.py [out.png]` does it and saves a 2× PNG. Other
  serial keys reach the firmware as `shell::Input::key` for dev shortcuts (Weather:
  `r` fetch, `n` next screen, `l` lock).
- "Port is busy" on upload means the user's VS Code serial monitor is open — ask them to
  close it rather than killing their process.
- Destructive disk operations (e.g. `diskutil eraseDisk`) are blocked for Claude: give the
  user the exact command to run with `!`.

## Architecture decisions

- **Launcher + cartridges** (plan agreed 2026-10-04; launcher skeleton done): one firmware per product concept
  (music, weather, …), never mixed; the iOS app swaps them over BLE. The launcher (ota_1)
  installs cartridges and shows progress with the cartridge's icon; Rescue (factory) installs
  and repairs the launcher. Shared code lives in `lib/dotty_core/src/`; each cartridge is
  `cartridges/<name>/` with its own `[env:<name>]` in `platformio.ini`
  (`build_src_filter = +<name>/`). Never add cartridge-specific code to dotty_core
  unless a second cartridge needs it.
- Flash layout 2 (`partitions.csv`, since 2026-10-06; migrating needs one USB flash of
  launcher + rescue + a cartridge, NVS stays): `rescue` = factory 768 KB (Rescue, 593 KB),
  `launcher` = ota_1 2 MB, `ota_0` 4 MB = the active cartridge, `storage` 1.1 MB (unused
  LittleFS), coredump still at 0x7F0000. Layout 1 had the launcher in factory and no Rescue;
  cartridges built for layout 2 can't find the launcher on layout 1. Cartridge envs upload
  to ota_0 with `boot_app0.bin` (boots the cartridge); `tools/pio_launcher.py` uploads the
  launcher to ota_1 with an otadata selecting it (seq 2; crc = `zlib.crc32(seq, 0xFFFFFFFF)`,
  same as boot_app0's), `tools/pio_rescue.py` uploads Rescue without touching otadata. The
  platform resets `ESP32_APP_OFFSET` to ota_0 during the build (pre-actions set it back) and
  copies FLASH_EXTRA_IMAGES into `UPLOADERFLAGS` early: swap boot_app0.bin in UPLOADERFLAGS
  in an upload pre-action (replacing FLASH_EXTRA_IMAGES did nothing — the old blank-otadata
  launcher upload never worked, which is why a USB-flashed launcher always booted the cartridge).
- **Rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on in the prebuilt bootloader):
  dotty_core overrides `verifyRollbackLater()` → true, so a firmware booted for the first
  time after `esp_ota_set_boot_partition` is on trial; the shell confirms it after 5 s
  (`cartridge::confirmHealthy`), and before deliberate restarts and power-off (else a quick
  power-off would roll back a good update). A crash before that → the bootloader marks it
  ABORTED and boots the other otadata entry (or factory).
- Every firmware declares `DOTTY_CARTRIDGE(id, name, version)`; the struct lands in
  `.rodata_custom_desc` at offset 0x120 of the image, where the launcher reads it
  (`cartridge::readInstalled`). `cartridge::rebootToLauncher()` / `startInstalled()`
  switch boot partitions.
- Lists (Music playlists, Album menu, Tape "Tapes", Joke favourites): 38 px rows (28 was
  too small to tap), always 4 a page; the page is the nav bar's right corner
  (`nav::draw(…, rightText)` + `nav::pageLabel`, "2/3"; tapping it = next page, round), so no
  pager row. Swipe left/up = next page, right/down = previous.
- Shared UI in dotty_core once two cartridges needed it: `nav_bar.*` (black top bar, 45 px
  since the user kept missing the 30 px bar's arrows; corner icons incl. stars; `nav::hit`
  and `nav::kTouch` = a third of the width per corner, the title isn't a button), the off pictures (`images/sleep_*.h`), touch
  swipes (`Touch::Gesture::Swipe*`, fired on release past 35 px; the e-paper can't follow a
  finger, so scroll by pages), and the lock-screen widget (`shell::Config::drawLockWidget`
  draws into its own 200x80 canvas and returns its height; the clock row — clock + a
  padlock as tall as the digits — and the widget are centred together). A widget must not
  touch the display's font (it shares `epd`): measure on the canvas it's given.
- A cartridge can replace the whole lock screen with `shell::Config::lockScreen(gfx, info)`
  (Album's photo screensaver); return true when the picture changed: photos get a full
  refresh (partial refreshes ghost on dithered photos), the clock-only minute a partial one.
- `lib/dotty_core/src/shell.*` owns the shared device behaviour (PWR lock/unlock/off,
  BOOT + PWR 1 s → launcher, auto-lock, lock screen + sleep, off picture). Firmwares
  pass a `shell::Config` (drawApp + optional hooks) and run their own logic only while
  `shell::update()` returns true.
- Launcher screens use the firefly logo on white, not the user's photos; cartridges keep
  the random portrait/panda off screen. The bitmap comes from
  `ios/Design/Logo/dotty-mark-epaper.svg`: the flat mark with a light-grey tail, black
  outline and segment stripes (the original pale-yellow tail dithers to almost nothing),
  no glow. Render with `qlmanage -t -s 800`, convert with img2epd (atkinson, 120 px).

- **Partition table is OTA-ready** (layout 2 above). The long-term goal is firmware updates
  from the iOS app; don't switch to a no-OTA layout.
- **Display driver** (`lib/dotty_core/src/epd_display.*`) subclasses `GFXcanvas1`: its buffer layout
  (MSB-first, 25 bytes/row, 1 = white) matches the controller RAM, so it's sent as-is.
  Init sequence and LUTs come from Waveshare's example driver. Draw with Adafruit GFX,
  then call `refreshFull()` or `refreshPartial()`. Do a full refresh every ~30 partials
  to clear ghosting.
- Pictures: convert with `tools/img2epd.py` (system `python3` has Pillow) into
  `cartridges/<name>/images/*.h`, draw with `drawBitmap(..., kBlack)` on a white background.
  Atkinson dithering with brightness ~1.15 / contrast ~1.4 suits photos on this panel;
  generate a few variants and compare the previews before picking.
- Pins live only in `lib/dotty_core/src/board_pins.h`; source them from Waveshare's
  `02_Example/Arduino/*/user_config.h` when adding peripherals.
- LVGL (8.3.11 / 9.3.0) is supported by Waveshare but not used yet; Adafruit GFX is enough
  for now on a 1-bit 200×200 screen.

## UX decisions

- Kindle-style lock: PWR short press locks/unlocks; lock screen redraws once a minute
  (panel wear + battery), music keeps playing; auto-lock after 2 min idle. While
  unlocked, 1 refresh/s is fine only when something is actively changing.
- Screens: lock = clock + padlock icon; power off = a random pick from `kOffPictures`
  in main.cpp (user's portrait, red panda illustration). Use the user's pictures, not
  drawn illustrations.
- e-paper datasheet: rated "panel life 5 years", refresh at least once per 24 h, no
  refresh-count rating. Keep full refreshes rare and periodic.

## Dotty Core BLE service (`lib/dotty_core/src/core_ble.*`)

- NimBLE-Arduino 2.x + ArduinoJson 7. Device name `Dotty-SP01` (fixed, `kDeviceName` in
  core_ble.cpp; it was `Dotty-` + MAC suffix, "Dotty-B100"; Info has it as `device`), the same
  in every firmware, so the app sees one device across cartridge swaps.
- Service `b9c10000-fbaa-4525-8400-055f7a543231`; characteristics `…0001` Info (read,
  JSON, refreshed every 10 s; **max 512 bytes** — a longer value is cut and the app can't
  parse it, which hid the Music screen in 0.8.0 at 525 bytes. Info lists command
  namespaces as `features`; `core.info` returns the full `commands` list. The app checks
  `DottyInfo.supports("wifi")` / `isAtLeast("0.7.0")`, never individual commands in Info), `…0002` Command (write, JSON `{"cmd": …}`), `…0003` Event
  (notify, JSON replies `{"cmd": …, "ok": …}`), `…0004` Data (write-no-response, for
  installs).
- Events longer than one notification (MTU − 3, ~290 B on iPhone) are split: every piece
  but the last starts with byte `0x1E`; clients append pieces until one doesn't (done in
  `DottyLink` and `ble_dotty.py`). Commands are written with response, max 512 bytes.
- Register commands with `ble::on("<cartridge>.<verb>", handler)`; handlers run on the
  main loop via `ble::poll()` (called by `shell::update`), never on the BLE task. Every
  command counts as interaction: it restarts the auto-lock timer (unlocked only). The app
  pings (`core.ping`) every 45 s while it's in the foreground and connected (ContentView), so
  Dotty doesn't lock — and drop Bluetooth — under an open app; in the background it stops. A
  handler that reboots must defer it until after its reply has been notified.
- Pairing: Info is open; Command/Event/Data need encryption + authentication, so the
  first command makes iOS pair. Dotty is DISPLAY_ONLY: it shows a random 6-digit code
  (shell pairing screen) that the user types on the phone. Bonded, MITM, LE Secure
  Connections; bonds in NVS shared by all firmwares. `core.forget` deletes all bonds.
  Each `ble::start` logs "N bonded devices stored"; a failed encryption logs its status
  (custom GAP handler). Once (2026-10-06, after a failed app backup) Dotty refused the bonded
  iPhone *and* Mac ("pairing failed (bonded 0)", macOS: "Failed to encrypt the connection")
  across a reset, although the NVS bond records were byte-identical to the working state
  (decoded with a quick parser of `read-flash 0x9000 0x5000`); reflashing the cartridge fixed
  it and it didn't come back. If it recurs, read those log lines before reflashing.
  bleak/macOS also gets a pairing prompt on the first command now.
- Core commands: `core.ping`, `core.info`, `core.forget`, `core.toLauncher` (cartridges only),
  `core.time {local, utcOffset?}` (registered by the shell: local epoch seconds; sets the RTC
  when ≥ 2 s off, replies `drift`; `utcOffset` = the phone's seconds east of UTC, kept in NVS
  `clock/utcOffset` → `shell::utcOffset()`, since Dotty's clock is local and UTC-dated things
  like news need the offset; the app sends it on every connection — `DottyLink.syncClock`;
  `ble_dotty.py cmd core.time` sends the Mac's time, taken after a ping so pairing doesn't
  make it stale);
  launcher: `launcher.start`, `install.begin/end/abort`; music: see below.
- Identity: advertisement = flags + service UUID + manufacturer data `0xFFFF` + 6-byte
  chip serial (factory MAC, e.g. 70:04:1D:D7:B1:00); name in the scan response. Info
  also has `"serial"`. iOS hides real MACs, so apps must use this serial.
- **Install protocol** (launcher only, `cartridges/launcher/installer.*`): see the
  header comment. Data writes are unacknowledged with a 4-byte offset prefix; the
  launcher drops out-of-order data and asks `install.resend {from}`; `install.end`
  may answer `missingFrom`; SHA-256 + `esp_ota_end` verify before the boot partition
  changes. Disconnect for 5 s aborts. Payload = 64×64 1-bit icon (512 B) + image.
  Clients **must wait for CoreBluetooth's `canSendWriteWithoutResponse`** before each
  write — writes sent while its queue is full are silently dropped (bleak doesn't
  check it; ble_dotty.py reads it from bleak's CBPeripheral).
- **Launcher updates and Rescue** ("Dotty system" panel on Cartridges): the launcher is in the
  catalog with `"system": true` (apps hide it from the cartridge list). `library.fetch
  {id: launcher}` (or `install.fromCard`) downloads it to the card, sets NVS `rescue/install`
  = version and restarts into Rescue (`cartridges/rescue/`, factory, USB-only, never updated
  by the app). Rescue writes the launcher slot from `/cartridges/launcher/firmware/<v>.bin`
  (checking the .json SHA-256 and `esp_ota_end`), and starts it on trial; the cartridge slot
  isn't touched (the app restarts the cartridge with `launcher.start`). Each launcher, 8 s
  into a healthy run, records NVS `rescue/good` = its version and copies itself to the card
  if it isn't there (`library::saveRunning`), so Rescue always has a good one. **Automatic
  rescue**: a launcher that crashes on trial is rolled back by the bootloader; the next
  firmware (a cartridge, via `shell::begin` → `cartridge::launcherBroken()` → ABORTED/INVALID)
  or the bootloader itself hands over to Rescue, which reinstalls `good` (else the newest
  ≥ 0.9.0 on the card that isn't the broken one). Rescue refuses launchers < 0.9.0 (layout 1
  ones copy themselves into factory). No good launcher anywhere → "Dotty needs a computer".
  Tested 2026-10-06: 0.9.0 → 0.9.1 through Rescue; a launcher that aborts in setup
  (`PLATFORMIO_BUILD_FLAGS=-DDOTTY_TEST_CRASH pio run -e launcher`, written to 0xD0000 with an
  otadata of {seq 1 ota_0 VALID, seq 2 ota_1 NEW}) → rolled back → Rescue put 0.9.1 back.
  Info has `launcher` (its version) in every cartridge. Only one Dotty exists (the
  prototype, never released): no compatibility with layout 1 is needed.
- **A firmware on trial can't write flash**: `esp_ota_begin` returns
  `ESP_ERR_OTA_ROLLBACK_INVALID_STATE` while the running app is PENDING_VERIFY. The launcher
  is on trial for its first 5 s after every switch from a cartridge, and the app installs
  right after switching, so the first install failed ("flash busy", the old message for any
  esp_ota_begin error) and a retry worked. installer/library now call
  `cartridge::confirmHealthy()` before `esp_ota_begin` and report the real error name.
- **Update outcomes reach the user**: Rescue writes NVS `rescue/result` ("failed" with
  `reason`, or "rolledBack") + `resultVer`, and `rescue/trying` for an update on trial, which
  the launcher turns into "updated" once it has run 8 s. The launcher's Info has `update`
  {status, version, reason?} and its home screen says "Update to X failed" until the app
  sends `launcher.updateSeen`. The app waits for "updated" (not just the version: a launcher
  can still fail its trial), explains failures ("didn't start, so Dotty went back to…"),
  keeps waiting when Bluetooth drops mid-download (Dotty carries on alone), and shows a
  failure it missed on the Dotty system panel. Tested with the crash build: Info said
  rolledBack 0.9.9 → back to 0.9.3.
- **Factory reset** (Rescue 1.2.0, launcher 0.9.4): app Dashboard › Settings › Factory reset
  (confirmation), or on Dotty: hold BOOT 10 s on the launcher home screen → "Erase
  everything?" Erase / Keep (20 s timeout; serial key `f` opens it for tests). BOOT held at
  power-on can't be the trigger: the chip starts in USB flashing mode. The launcher sets NVS
  `rescue/reset` (+ `rescue/install` = the newer launcher the app downloaded first with
  `library.fetch {install: false}`) and restarts into Rescue, which: copies the running
  launcher (and the newer file) into PSRAM, formats the card (FAT32, 32 KB clusters, via IDF
  `esp_vfs_fat_sdcard_format_cfg` — the Arduino SD_MMC wrapper hides the card handle; a
  broken file system is formatted too), `nvs_flash_erase()` (pairings, Wi-Fi, every
  setting), erases the ota_0 header, writes the launcher(s) back to the card, sets
  `rescue/good` (and `trying` for the newer one) and starts the launcher. The app forgets
  Dotty (`DottyLink.forgetAfterReset`) and the pairing screen asks to Forget This Device in
  iOS Settings › Bluetooth (apps can't remove iOS bonds). Run for real on 2026-10-06:
  reset → welcome → pairing (after Forget This Device) → Wi-Fi → restore of the 152 MB iPhone
  backup (after the NFC fix) → Music back with 29 songs and 4 playlists.
- **Welcome flow** (launcher home, user-facing, no jargon): never paired (`ble::bondCount()`
  0) → firefly + "Hi, I'm Dotty! / Open the Dotty app / and pick Dotty-SP01"; paired, no
  Wi-Fi → "We're friends! / Next, pick a Wi-Fi network in the app"; `wifi.add` → "One
  moment... / Joining Naru" (`net::onJoining` hook; the refresh runs while it blocks) and
  "Hmm, no luck / Couldn't join … / Check the password in the app" for 6 s if it failed;
  no cartridge → "All set! / Choose a cartridge in the Dotty app"; then the usual home
  ("Music is ready / Press BOOT to start"). Pairing screens (shell, every firmware): black
  title bar "Pairing", "Type this code on your iPhone", the code, the name; then a tick +
  "Paired! / Nice to meet you" or "That didn't work / Try again from the app". Dev keys: `w`
  steps through the welcome screens, `p` shows the pairing screens.
- **SD card backup to the Mac** (`tools/card_backup.py backup [folder]` → `~/Dotty Backups/
  <date time>/`, `restore <folder>`): switches to the launcher, `transfer.start {scope: card}`
  (whole-card mode, launcher only: `GET /card/list`, `GET /download?path=`, `POST
  /upload?path=` with absolute card paths, no ".."), checks every size, `POST /done`. The
  Mac must be on Dotty's Wi-Fi, and the iPhone app must not hold Dotty's one BLE link.
  Default = data only (`--with-firmware` adds the cartridge copies; the catalog has them all).
  **Smart restore**: `GET /card/list?hash=1` (streamed with chunked responses — fingerprinting
  160 MB takes minutes, a silent reply looks dead) gives every file's SHA-256; only new or
  changed files are sent, files the backup lacks are deleted (`POST /card/delete?path=`),
  never the launcher's copies, hidden files, or firmware copies for a data-only backup.
  `--dry-run` shows the plan. First backup: 180 files (85 + the Mac's Spotlight files),
  167 MB in 472 s (362 KB/s); a restore after deleting one file sent just that file.
  ArduinoJson `serializeJson(doc, String&)` *replaces* the string (the first listing broke).
  **In the app**: Dashboard › Settings › Backups (`Features/Backup/`): back up (data only, or
  with cartridge copies) into Documents/Backups/<yyyy-MM-dd HHmm>/ with the same
  dotty-backup.json as the Mac tool (Files app: `UIFileSharingEnabled` +
  `LSSupportsOpeningDocumentsInPlace` in Dotty-Info.plist), list them ("29 songs · 8 photos
  · 2 recordings · 158 MB"), restore with the same smart diff (the iPhone's SHA-256s are
  computed off the main thread), delete. **Local paths are NFC-normalized**
  (`localFiles`): iOS enumerates the backup folder with decomposed names, which put a Korean
  song's upload URL at 562 characters (> 512, rejected by esp_http_server with no log) and
  stalled the first real restore after a factory reset; NFD paths also never match Dotty's, so
  the diff would resend and delete them. A file Dotty refuses (non-200) is skipped and listed
  instead of ending the restore; 409 still means cancelled on Dotty. The cartridge that was running starts again after.
  **Dotty's screen during a backup/restore** (launcher `drawCardTransfer`): title Backup /
  Restore, "20 of 50", a bar by size and the file without its extension. Dotty can't know
  the plan by itself (the client picks the files), so each request carries headers
  `X-Dotty-Job: backup|restore`, `X-Dotty-Step: 20/50`, `X-Dotty-Bytes: <before>/<total>`
  (transfer.cpp `readProgress`); while it fingerprints for a restore it shows "Checking my
  files · N files checked". Hidden files are skipped by both the app and the Mac tool.
  `ble::bondCount()` caches the count: reading NimBLE's store while BLE is off (locked, or
  paused for Wi-Fi) asserts in ble_hs_lock — it crashed the launcher mid-restore.
  **Per-cartridge backups**: the app lists the cartridges on the card (`storage.list`, with
  sizes; all ticked by default) and backs up only the ticked ones' `/cartridges/<id>/`
  (Mac: `backup --only weather,jokes`); the launcher's own copies are never backed up. The
  manifest lists `cartridges` (older backups: every cartridge they have files of), and a
  restore only adds/changes/deletes inside those, so a Weather-only backup leaves Music alone.
  **Cancel** (app progress card, or the Cancel button at the bottom of Dotty's transfer
  screen, y > 142): the file in flight is dropped at once — the user found "finishing this
  file first" unresponsive — (a cut-off chunked download; an upload's `.part` deleted, then
  409 + `ESP_FAIL`, because returning ESP_OK makes esp_http_server read and discard the rest
  of the body first). Later requests get 409 "cancelled on Dotty" and `GET /status` says
  `cancelled`, so clients report "stopped on Dotty"; the server stays up until nothing has
  moved for 5 s (stopping right after the last file left the Mac timing out against a server
  that was gone). The app deletes a half-made backup folder (the Mac tool too); a cancelled
  restore keeps the files already sent. Tested: Mac backup and restore both stopped within
  ~1 s of the tap.
- **Catalog** (`tools/build_catalog.py`): builds every cartridge and the launcher into
  `dist/` (`<id>-<version>.bin` + `catalog.json`: id, name, version, description,
  requires, size, sha256, firmware URL, base64 512-byte icon; root has `format`,
  `protocol` = install protocol version, `release` tag). `--release` publishes a GitHub
  Release marked latest (needs a clean, pushed tree); the app reads
  `https://github.com/Mordokay/Dotty/releases/latest/download/catalog.json`. Bump the
  version in `DOTTY_CARTRIDGE()` when a cartridge changes **and publish a new catalog**:
  the app installs whatever the latest release has, so a stale catalog silently
  downgrades devices (happened: Music 0.5.1 replaced a USB-flashed 0.6.0). Run it with
  `.venv/bin/python tools/build_catalog.py --release` (needs Pillow); every cartridge change
  ends with commit → push → catalog release, without waiting for the user to ask. Optional per-cartridge
  `cartridges/<id>/cartridge.json` = description + requires.
- Two pictures per cartridge: `icon.png` → 64×64 1-bit for Dotty's install screen (base64
  in the catalog), and `artwork.png` → square full-colour picture for the app (published
  as `<id>-<version>.png`, scaled to 512, catalog field `artwork` = URL; the app falls back
  to the pixel icon). Keep the editable `artwork.svg` next to it, in the app icon's
  night-sky/firefly style; render with `qlmanage -t -s 512` (no rsvg/magick on this Mac).
- Measured from the Mac: 2M PHY, ~30 ms interval, 7-9.5 KB/s (856 KB music ≈ 90 s;
  occasionally much slower right after another transfer). Expect better from iOS.
- The lock screen (clock once a minute) is drawn by whichever firmware is running — each
  has the shell — not by the launcher in the background. Periodic locked work is per
  firmware: `shell::Config::lockedWakeSeconds` + `onLockedWake` (return true to redraw the
  lock screen); the shell sleeps until the next minute or the next app wake, whichever
  comes first (e.g. Weather fetching every 10 min).
- Bluetooth while locked is per firmware: `shell::Config::bluetoothWhileLocked`.
  false (default; launcher, music, weather): locking stops BLE so Dotty sleeps; unlock
  restarts it. true (e.g. a future ANCS notifications cartridge): a connected phone stays
  connected while locked (Dotty stays awake while connected). OFF always cuts BLE.
  Advertising happens whenever BLE is on and nothing is connected.
- Power: wake lock `kWakeLockBle` while connected. Before light sleep the shell calls
  `ble::stop()` (NimBLE deinit; the controller can't sleep without a 32 kHz crystal) and
  `ble::start()` on unlock — so Dotty is only reachable while unlocked or awake.
- Test from the Mac: `.venv/bin/python tools/ble_dotty.py scan|info|listen|catalog|cmd <name> k=v k:=json|install <bin or id>`
  (`install --corrupt` checks the SHA-256 rejection). Cartridge icons: `cartridges/<id>/icon.png`.
  (venv setup in the script's docstring; bleak; Bluetooth permission already granted).

## SD card layout (`lib/dotty_core/src/storage.*`)

- Every cartridge owns `/cartridges/<id>/`: `firmware/<version>.bin/.json/.icon` (kept by
  the launcher) and `data/` (the cartridge's own files; `storage::myDataDir()`).
- `storage::begin()` mounts the card and migrates older layouts once (firmware files at
  the top of `/cartridges/<id>/` → `firmware/`, `/music/*` → music `data/library/`).
- Removing a cartridge (`storage.remove {id, data}`, every firmware): deletes
  `firmware/`; with `data` also `data/` and the NVS namespace named after the cartridge id
  (cartridges keep their settings there, e.g. `music/shuffle`); without it the data stays
  for a reinstall. A firmware can't remove itself (the app switches to the launcher first);
  the launcher's `storage::onRemove` hook empties ota_0 (`cartridge::eraseInstalled`) when
  the installed one goes. `storage.list` → {cartridges: [{id, versions, data}], free}.
- The system clock follows the RTC (set on every `RtcClock::read/write`), so SD file dates
  are local time; songs report them as `added`.
- Names from the phone go through `storage::safeName` (one path segment, no dots first,
  ≤ 120 bytes keeping the extension and whole UTF-8 characters — cutting `.mp3` off hid
  long-named songs from the library). The app mirrors it in `SongOutbox.storedName`.

## Wi-Fi file upload (`lib/dotty_core/src/transfer.*`)

- Files from the phone go over the home Wi-Fi, not BLE (~20-50x faster). `transfer.start`
  → Dotty joins its best saved network, starts `esp_http_server` on port 80, replies
  `{url, token, ssid, bluetooth: "paused"}` (16-byte random token, only sent over encrypted
  BLE), then **pauses BLE** ~0.4 s later: Wi-Fi and BLE share the radio and uploads run at
  about half speed with BLE on (measured from the Mac, 4 MB: ~130 KB/s with BLE connected,
  ~240 KB/s without; a 200 ms BLE interval, `esp_coex_preference_set(WIFI)` and
  `WIFI_PS_NONE` made no difference). The rest of the limit is the prebuilt core's 5.7 KB
  TCP window (`CONFIG_LWIP_TCP_WND_DEFAULT`): a custom core build could raise it. Phone:
  `POST <url>/upload?dir=<folder>&name=<file>` with header `X-Dotty-Token` → `{ok, name}`
  (**POST, not PUT**: iOS silently re-sends a PUT whose connection drops — one song went
  four times in a stress test); an upload with no data for 30 s is abandoned; the file lands in
  `<myDataDir>/<dir>/<name>` (via `.part`, renamed when complete). Query values are
  url-decoded with `+` = space, so the app percent-encodes everything but unreserved ASCII.
- `POST <url>/done` (token header; the app's normal ending since BLE is paused),
  `transfer.stop`, or 1 min idle stops the server and Wi-Fi and restarts BLE (unless locked);
  the phone's pending connection reconnects by itself. The cartridge's `onFinished` gets a
  `transfer::Summary` (received, failed, appFinished); Music shows "Songs received" or
  "Transfer failed" for 4 s, then the player. `.part` leftovers are deleted when a session
  starts. Wi-Fi signal matters most (small antenna): at −85 dBm a stress test ran at 95–166
  KB/s; next to an iPhone hotspot the user measured > 500 KB/s. `transfer.start` replies
  `rssi`; below −75 dBm the app suggests the hotspot.
- Downloads (Tape): `GET <url>/download?dir=&name=` with the token streams a data-folder file
  in 16 KB chunked responses, borrowing an upload block (a static 16 KB buffer left the tape
  cartridge without internal RAM for the upload blocks: "not enough memory for the transfer").
  Measured 100 KB/s at −45 dBm; at −82 dBm only ~10 KB/s.
- `esp_http_server` rejects URIs over 512 characters (`CONFIG_HTTPD_MAX_URI_LEN`) before any
  handler runs, so Dotty never logs it. iOS gives **decomposed** (NFD) file names — Korean
  ~1.7× longer — so the app sends `SongOutbox.storedName` (NFC, ≤ 120 bytes), keeping
  every upload URL under ~390 characters. Card writes run on a separate task
  (3 × 16 KB blocks) so they overlap receiving. Holds the Network wake lock meanwhile. Events `transfer.received {dir, name, size}`. Cartridges opt in with
  `transfer::registerCommands(onFinished)` and call `transfer::poll()` in the loop.

## Music cartridge (`cartridges/music/`)

- Data: `data/library/*.mp3` (every song once) and `data/playlists/<name>.m3u` (`#EXTM3U`
  + lines `../library/<song>`, so the card works in computer players). `music_library.*`.
- Queue = the library or one playlist, played in order or shuffled (`order` over the
  queue, current song first; shuffle saved in NVS `music/shuffle`). Next/previous
  (previous restarts the song after 3 s), auto-advance to the end of the order, BOOT = next,
  volume row at the bottom of the screen.
- Screens: player with a nav bar (left = shuffle/in-order toggle, title = "Music" or the
  playlist, right = playlists) and the playlist menu (back; "Play all" + playlists with
  song counts, current one inverted, paged with a `< 1/2 >` row); picking one plays it and
  pops back to the player.
  A receiving screen shows while a Wi-Fi transfer is active.
- Screen text goes through `ui::printable` (fonts are ASCII only: curly quotes/dashes are
  mapped, Korean/Chinese dropped with their empty brackets; all-foreign titles fall back
  to "Song N"). Player title = one line + "4 / 15" below; the receiving screen wraps to 2
  lines (`ui::drawWrapped`).
- Commands: `music.status`, `music.toggle`, `music.next`, `music.prev`, `music.volume
  {value}`, `music.shuffle {on}`, `music.play {playlist?, index?, song?}`, `music.library` (songs + playlists),
  `music.playlist {name}`, `music.playlist.create/delete {name}`, `music.playlist.rename {name, to}`, `music.playlist.move {name, from, to}`, `music.playlist.sort {name, by: name|added}`, `music.playlist.add
  {name, songs[]}`, `music.playlist.remove {name, song}`, `music.song.delete {name}`,
  plus `transfer.*`. Events: `music.state` (on change, every 5 s while playing),
  `music.library` (library or playlists changed).

## Joke Factory cartridge (`cartridges/jokes/`)

- Keeps **every English JokeAPI joke** (~319, ~45 KB) on the card (`joke_store.*`):
  `data/jokes.tsv` (id, category, setup, punchline; parsed in place in PSRAM),
  `data/seen.tsv` (id → last shown, local epoch), `data/favourites.json` (full text, so a
  favourite survives JokeAPI dropping it). A background task (12 KB stack, core 0) refreshes
  it via `/joke/Any?lang=en&amount=10&idRange=a-b` batches (32 requests; the last English id
  comes from `/info`): right away when empty (retry 10 min), then weekly (retry 1 h); a
  download with < 100 jokes is discarded. **No safe mode, no blacklist flags** (user's
  choice). Measured: Programming 80, Pun 84, Dark 71, Misc 60, Christmas 14, Spooky 10.
- Picking: a random unseen joke of the category, else the one seen longest ago (so they
  come back after a while). Shown on Dotty = seen; lock-screen jokes don't count.
- Screens: 2x2 grid (Dark, Code = Programming, Misc, Any; icon above label, labels ≤ 6
  chars; nav ★ = favourites), joke view (two-part: punchline on tap or BOOT, then the next
  joke; ☆/★ in the nav; swipe up/down scrolls 6 lines with ▲▼ markers), favourites
  (numbered, paged; a favourite's title is "14/117", BOOT = next), "No jokes yet" screen
  (needs Wi-Fi / no internet / download progress).
- Lock screen: a joke that fits 4 lines (150 of 319), the same per 5-minute slot of the
  clock (`mix(days*288 + slot) % fitting`), setup regular + punchline bold.
- BLE: `jokes.status` {count, unseen{Dark,Programming,Misc,Any}, favourites, syncing, done,
  total, syncedAt, syncError?}, `jokes.favourites`, `jokes.favourite.remove {id}`,
  `jokes.sync`; event `jokes.changed`. App: `Features/Jokes/JokesView.swift`.

## Weather Station cartridge (`cartridges/weather/`)

- Open-Meteo forecast (free, no key; `current` + 7 `daily` incl. max wind and its dominant
  direction, `timezone=auto`, imperial = `temperature_unit/wind_speed_unit/precipitation_unit`
  params) for a location that is automatic (ipwho.is from Dotty's public IP — off by a city on
  a phone hotspot) or set from the app: the iPhone's location (CoreLocation, named by MapKit
  reverse geocoding) or any address/place picked on an Apple Maps sheet (`PlacePicker.swift`:
  `MKLocalSearchCompleter` suggestions as you type, the map follows the best match after a
  350 ms pause, MKLocalSearch resolves it). Open-Meteo takes any coordinates (gridded
  models), so every MapKit result works; Dotty's title gets the town (`cityName`). Settings
  in NVS `weather` (auto, lat, lon, name, source = phone|city, imperial). Fetched hourly (retry 10 min) by a background task (core 0, 12 KB), also
  while locked; the raw answer is cached as `data/forecast.json` + `forecast.meta`.
- **No inside reading**: the board's SHTC3 (I2C 0x70) read 29 °C in a 21 °C room — Dotty's
  own heat, varying with Wi-Fi/charging/screen, so a fixed offset can't fix it. Removed in
  0.2.0 with its driver (`climate.*`, in git history before that commit) and the app setting.
- Every fetch also syncs the clock: `net::internetTime()` (SNTP, read the sync status once
  per check — reading clears it) + the forecast's `utc_offset_seconds` →
  `shell::setLocalTime()` (writes the RTC). The fetch task only hands results over
  (`takeFetched`, `takeClock`); drawing stays on the main loop.
- Screens: Today (place in the nav + "3h ago" when stale; 56 px icon, big temperature,
  condition; TODAY low/high | FEELS LIKE; UV, wind, rain chance, next sunset/sunrise), then
  two "Next days" pages of 3 cards (tomorrow onward: 44 px icon; day + low/high; rain chance
  + the day's total in words: Dry < 0.2 mm, Light < 4, Moderate < 15, else Heavy — the user
  found "1.6mm" meaningless and mixed rows of mm/UV untidy; wind sign + strongest wind).
  Swipe or nav arrows; BOOT cycles. Icons are drawn from WMO codes (`weather_icons.*`;
  clouds are outlined by insetting each part, and a cloud over a sun/moon clears a gap of
  its own shape). Lock widget = a card: 48 px icon, "23°  19/26°", rain chance + words,
  sunset/sunrise.
- BLE: `weather.status` {location{automatic, name?, lat?, lon?, source?}, units, now?{place, temp,
  feels, humidity, code, description, wind, uv, fetchedAt}, fetching, error?},
  `weather.location {automatic, lat, lon, name, source: phone|city}`, `weather.units {units: metric|imperial}`,
  `weather.refresh`; event `weather.changed`. App: `Features/Weather/WeatherView.swift`
  (location permission text = `INFOPLIST_KEY_NSLocationWhenInUseUsageDescription`).

## Album Viewer cartridge (`cartridges/album/`)

- Photos are made on the phone (`Features/Album/PhotoDither.swift`): the user frames a square
  in `PhotoEditor` (pinch/drag, rule-of-thirds guides, brightness/contrast sliders, live
  "On Dotty" preview), then 200x200 grey → brightness · contrast (Pillow's ImageEnhance
  maths) → Atkinson; defaults 1.15/1.4 like `tools/img2epd.py`. Named after the EXIF date
  taken, `20240714-193205.pbm` (`-2`… if taken), so name order = date order and Dotty shows
  "14 Jul 2024" from the name.
- Card (`album_library.*`): `data/photos/*.pbm` (binary PBM P4, 200x200, 1 = black, 5011 B —
  opens on a computer; the bitmap is what `drawBitmap` wants), `data/albums/<name>.txt` (one
  photo name per line, album order). Deleting an album keeps its photos; deleting a photo
  drops it from every album. "All photos" = every photo by name.
- Uploads: Wi-Fi `transfer` (dir `photos`), sent right after the editor closes
  (`PhotoOutbox`, queue in Application Support/PhotoOutbox survives closing the app), then
  `album.add` for the album they were added from (batched: commands ≤ 512 bytes).
- Dotty: viewer = the photo full screen; tap shows the album name (nav, back = album menu)
  and "2 / 4 · 14 Jul 2024" for 6 s; swipe or BOOT = next; a new photo = full refresh. Album
  menu like Music's playlist menu. NVS `album`: active album, photo, saver mode/photo.
- Lock screen (`Config::lockScreen`): the active album as a slideshow — from the photo on
  screen, a new photo every 10 / 30 (default) / 60 min on the clock (:00, :30…; NVS
  `album/every`, `album.slideshow {every}`), looping. Every minute (0.1.0) was dropped for
  panel wear: each photo change is a full refresh, and those wear e-paper (the datasheet has
  no refresh-count rating); at 30 min it costs no more than the plain clock lock screen's
  own ghost-clearing full refresh every 30 partials;
  unlocking stays on that photo — or always one photo; a white badge bottom-right with a
  padlock, the time, and the battery when low or charging. No photos = the usual clock.
- BLE: `album.status` {album, index, count, photo, screensaver{mode, photo?}, photos,
  albums}, `album.library` {photos[{name, added}], albums[{name, count, cover}]},
  `album.album {name}`, `album.photo {name}` → base64 bitmap (the app caches it in
  Caches/AlbumPhotos and fetches thumbnails one at a time), `album.show {album, photo?}`,
  `album.screensaver {mode: album|photo, album?, photo?}`, `album.create/delete/rename/add/
  remove/move`, `album.photo.delete {names[]}`, plus `transfer.*`; events `album.state`,
  `album.library`. Dev keys: `n` next, `o` labels, `l` lock.
- App previews (`EpaperImage`): shrunk = smoothed; enlarged = a whole number of screen
  pixels per dot (uneven nearest-neighbour scaling looked worse than the real panel).
- App: `Features/Album/` — tabs Photos (On Dotty card with the slideshow interval, grid with multi-select: add to album /
  delete) · Albums (covers; album page: show, lock-screen slideshow choice, add new or
  existing photos, drag a photo onto another to reorder, rename, delete).

## Tape Recorder cartridge (`cartridges/tape/`)

- `recorder.*`: a tape = one WAV (32 kHz, 16-bit mono, ~3.8 MB/min) in `data/recordings/`,
  named after its start time (`20261006-091412.wav`; a rename keeps `.wav`). record() and
  pause() alternate on the same tape; finish() closes it (an empty tape is deleted). A
  reader task (core 0) pulls `AudioPlayer::capture()` blocks into a 128 KB PSRAM stream
  buffer, a writer task (core 1) moves them to the card; pausing writes the header length
  so a power cut keeps everything up to the last pause; boot repairs headers that disagree
  with the file size. The I2S port runs at 32 kHz for the whole cartridge (TX and RX share
  its clock: `AudioPlayer::begin(rate)`).
- Controls: **BOOT held ≥ 120 ms = record, released = pause** (PWR is untouched: short =
  lock, 2 s = off — the user first thought of PWR, which can't double as record). While
  recording: the deck stays up, `shell::wake()` every pass (no auto-lock); locking anyway
  pauses. Paused: may lock; the lock screen's bottom line says "Tape paused 0:42" (on a
  charger too — the big battery already shows charging); BOOT does nothing while locked;
  unlocking with a tape open returns to the deck. Paused row: ↶ undo (drops the last part —
  each BOOT hold is a part; the file is cut back with VFS `truncate("/sdcard/…")`, the hint
  says "Undo takes the last 0:06"), Save, 🗑 (the cassette turns into "Discard this tape?"
  Yes / No; gone after 10 s or when BOOT records again). Text labels didn't fit three wide.
  Player: 🗑 in the nav's right corner (`nav::Icon::Trash`) pauses and asks "Delete this
  recording?" Yes / No in place of the controls; Yes shows the next one, No plays on.
- Screens: deck (cassette, ● REC time + 20-bar dB meter -60..0 dBFS, or PAUSED + Save),
  recordings (paged, newest first, "Mon 6 Oct 09:14" + length), player (‹ › and swipes =
  previous/next, volume row). Dev keys: `r` toggles a simulated BOOT hold, `v` saves, `w`
  dumps the newest recording over serial as hex between `#WAV <size> <name>` / `#END`, `l`.
- BLE: `tape.status` {state: idle|recording|paused, elapsed, playing?, paused?, position?,
  volume}, `tape.list` {recordings[{name, title, size, duration, added}], rate},
  `tape.play/toggle/stop`, `tape.volume {value}`, `tape.rename {name, to}`, `tape.delete
  {name}`, `tape.undo` / `tape.save` / `tape.discard` (paused tape; status then has `parts`,
  `lastPart`), `transfer.*`; events `tape.state`, `tape.list` (also sent on save).
- App (`Features/Tape/`): rows with ▶ (fetches the WAV over Wi-Fi — `transfer.start`, `GET
  /download?dir=recordings&name=…`, `/done` — into Caches/Recordings, then AVAudioPlayer),
  share/save (ShareLink), menu: play on Dotty, rename, delete; a deck card while Dotty
  records or plays.

## News cartridge (`cartridges/news/`)

- Topics (max 12, managed in the app), three sources:
  - **Outlet feeds** (`news::feeds()`, key `s:<id>`): BBC sections (short ids top, world… —
    the first topics), NYT, Washington Post, NBC, ABC, CBS, NPR, Fox, Bloomberg, FT, Guardian,
    Al Jazeera, Sky, Euronews, TechCrunch, Ars, The Verge (Atom). Checked 2026-10-06: CNN's and
    WSJ's feeds stopped updating (2023 / early 2025), MSNBC's is empty (NBC's works), Reuters
    and AP have none → keywords like `site:wsj.com` (headlines only).
  - **Kagi News daily briefings** (key `c:<file>`, e.g. `formula_1.json`; ~188 categories, the
    app reads https://kite.kagi.com/kite.json itself): the day's ~12 big stories, each clustered
    from 40-60 outlets with an AI summary; once a day (~12:00 UTC). Files are 100-450 KB: saved
    to the card (`kagi.part`), then read back with an ArduinoJson filter (title, short_summary,
    unique_domains); source shown as "50 sources". The index timestamp is kept (NVS
    `news/kagi`): the same edition isn't downloaded again. Ages count from the file's own
    timestamp; their score's age term is capped at 12 h. Licence believed non-commercial.
  - **Keywords** (key `k:<query>`): Google News search RSS, `when:2d`, en-GB: headline + source
    (title's " - Source" suffix split off), no summary.
  Summaries are cut to whole sentences, ≥ ~80 and ≤ 250 chars ("Oct." / "U.S." aren't ends;
  Kagi's `[site#1]` marks removed). ★ = favourite. Starter set: Top stories ★, Tech ★, World,
  Science. Files in `data/`: `topics.json` ({section | kagi | query, name, star}),
  `stories.tsv` (topic key, published UTC, feed position, source, title, summary; parsed in
  place in PSRAM like jokes.tsv).
- Fetch task (`news_store.*`, core 0, 12 KB) every 30 min (also locked: the loop runs at every
  minute wake, Network wake lock while fetching), 5 s after a topic add/remove, retry 10 min.
  RSS is parsed while streaming (`<item>` by `<item>`; CDATA, entities, tags stripped), stopping
  after 20 items (Google's feeds are 130 KB); a topic keeps its 10 best stories ≤ 48 h old; a
  topic whose feed fails keeps its old stories. Measured: 4 topics, 36 stories in 11 s.
- Ranking: score = feed position + 0.5 × hours old − 2 per extra topic carrying the same
  title (a big story); Favourites = starred topics (all if none starred), deduplicated.
- Dotty: menu (★ Favourites + each topic with counts, 38 px rows, paged), story screen
  (headline bold, "BBC - 2h ago" with a rule under it, summary; swipe up/down scrolls, with a
  scrollbar on the right edge — dotted track, thumb sized to the visible part — instead of ▲▼,
  swipe left/right / BOOT / the "3/10" corner = next/previous). Lock widget: a favourite
  headline that fits 3 bold lines (measured, like jokes) + source/age, the top 12 taking turns
  per 5-minute slot (partial refresh). Ages need the phone's UTC offset (core.time).
- BLE: `news.status` {stories, fetching, done, total, fetchedAt (local), error?, topics[{key,
  name, section|kagi|query, star, count}]}, `news.sections` (outlet feeds: {id, outlet, section,
  name}), `news.topic.add {section | kagi + name | query, name?, star?}`, `news.topic.remove {key}`, `news.topic.star {key, on}`, `news.topic.move
  {from, to}`, `news.refresh`, `news.stories {topic?, limit?}`; event `news.changed`. Dev key
  `r` fetches now. App: `Features/News/NewsView.swift` (topics with ★ and a menu; "Add a
  topic": Daily briefing / News outlet → `TopicPicker` sheet (search, several at once), keyword
  field; "Favourites on Dotty now").

## Pet cartridge (`cartridges/pet/`)

- A virtual pet with the **1996 Tamagotchi (P1) rules**, our own species/art/touch/sounds.
  Source of truth: the P1 ROM datamine at rhubarbtart.neocities.org/p1hackinglog (`stats.txt`
  per-character timers, `tama_notes.txt` RAM map + evolution vectors, `timeline.py` simulator,
  `utils.py` discipline routine and old-age step 0xF3D). Bandai's ROM, sprites and names are
  never used (public repo); avoid "Tamagotchi"/"-gotchi" (trademarks).
- `pet_engine.*`: plain C++ (no Arduino). Per *awake* minute: heart timers, discipline checks
  (every Nth heart drop: `m214 = discipline + m214 + 1`, call unless it rolls past 15), poop
  (180 min; baby 15/25), natural sickness timer, evolve timer (not while sick), age +1 each
  morning, lights call at bedtime, 15-min call windows → care / discipline mistakes, the
  datamine's evolution vectors, adult mistakes (5 = death), 3 sicknesses per stage = death (that
  is also old age: adult life ≈ 3 × its sickness timer), old adults' heart intervals shrink each
  morning (0xF3D). ★ Unsettled in the datamine, kept as constants: lights mistake after 15 min
  (ROM guess 64), starvation 720 awake min, untreated sickness 720, poop sickness after 120 min
  or 4 poops, a snack takes 1 min off the sickness timer.
- **Verified on the Mac**: `python3 tools/pet_sim/compare.py` downloads the datamine's simulator
  (feeding it stats.txt instead of the ROM via a stub `rip.py`), runs it and `sim.cpp` (our
  engine, perfect care) for every character from 3 start times: all 36 timelines match (the
  only exception, cut by the script: the datamine still counts the old character's heart in the
  minute it evolves/dies). Then `tests.cpp`: every evolution branch, each death, pause, food,
  game, the old-age step. Run it after any engine change.
- Species (P1 order → ours): Babytchi Dotlet, Marutchi Puffle, Tamatchi Sprig, Kuchitamatchi
  Lumpkin, Mametchi Lumo (best), Ginjirotchi Bramble, Maskutchi Masko (→ secret from a type-2
  teen), Kuchipatchi Chompy, Nyorotchi Wiggle, Tarakotchi Spike, Bill Sir Moss.
- Art: `tools/pet_art.py` → `images/sprites.h` (+ `tools/pet_art_preview.png`, `icon.png`,
  `artwork.svg`): 32×32 creatures from shapes + a shared face kit (idle, bob, blink, happy, sad,
  angry, eat, no), egg (3 frames), angel, 16×16 icons as ASCII grids. Drawn ×3 (home) / ×2 (lock).
- Dotty: home = pet (walks about for 20 s after a touch, 1.5 s frames), poop 2×2, top bar Food ·
  Light · Game · Medicine, bottom Clean · Meter · Discipline · bell (dark while calling). Screens:
  Food (meal/snack, bites), Light, Game (5 rounds left/right, decided before you choose, dots per
  round), Meter (age, weight, discipline bar, hearts, Pause + Sound buttons), animations for eat,
  refuse, medicine, clean (duck), scold, hatch, evolve, game end; death = angel + age + cause, tap
  for a new egg. Lock screen (`Config::lockScreen`, steps the engine each minute wake): small
  clock + battery, pet ×2 top-right, alert icons under the clock, three big rows (hunger hearts,
  happy hearts, discipline gauge 4×25 %), what it needs in a band (dark when action is needed).
- It lives only while this cartridge runs: on boot `lastMinute = now` (power off / another
  cartridge = paused). Saved as raw `State` in `data/pet.bin` (magic + size; bump
  `kStateVersion` when the struct changes) on events/actions and every 10 min;
  `data/history.tsv` per pet that died.
- Sound: `AudioPlayer::playNotes` (dotty_core) synthesises square-wave tunes with a plucked
  envelope (no files); tunes in main.cpp (call chirp, eat, happy, win/lose, evolve, hatch, poop,
  sick, medicine, clean, scold, no, death, tap click). One chirp per call, never repeated;
  locked + asleep: the codec is powered up for the chirp and down again. NVS `pet`: sound,
  volume, quietFrom/quietTo (minutes; default 23:30–08:00 so bedtime calls still sound).
- BLE: `pet.status` (species, stage, generation, age, weight, hunger, happy, discipline %,
  mistakes, sick, poops, asleep, lightsOn, paused, calling, needs, died?, sound{}),
  `pet.pause {on}`, `pet.sound {on?, volume?, quietFrom?, quietTo?}`, `pet.history`,
  `pet.newEgg {force?}`; event `pet.changed`. App: `Features/Pet/PetView.swift`.
- Dev keys: `F` fast-forward (~20 min/s, saved when stopped), `E` evolve now, `q w e r` / `z x c
  v` = the eight functions (`p` is the shell's pairing preview, `k` lock).

## SD library + Wi-Fi fetch (launcher)

- `cartridges/launcher/library.*`: cartridges on the card as `/cartridges/<id>/firmware/<version>`
  `.bin/.json/.icon`. Every BLE install is copied there (verified). `install.fromCard
  {id, version?, sha256?}` flashes from the card in ~2.5 s (whole region erased up front;
  4 KB sequential erases took 10 s). `library.list` lists the card.
- `lib/dotty_core/src/net.*` (every firmware): up to 8 saved networks as JSON in NVS
  namespace `wifi`; `net::connect()` scans and joins the strongest known one. HTTPS via
  `esp_http_client` + `esp_crt_bundle_attach` (real certificate checks), manual redirect
  loop (GitHub release downloads redirect). Wi-Fi is on only while something needs it.
- Preferred network (NVS `wifi/preferred`, "" = automatic): `connect()` tries it first when
  it's in range, then the rest strongest first. The last joined network and its RSSI are
  kept (`wifi/last`, `wifi/lastRssi`) so the app can show what Dotty uses even though Wi-Fi
  is off most of the time.
- BLE: `wifi.scan` → {ssid, rssi, secure, known}, `wifi.add {ssid, password}` (joins to
  check, saves only on success), `wifi.list` → {networks, preferred, last?, current?} (no
  radio use; the dashboard calls it on connect), `wifi.prefer {ssid}` ("" = automatic),
  `wifi.remove {ssid}`, `wifi.status`;
  launcher `library.fetch {id, version?, sha256?, install? = true}`: catalog over HTTPS → download
  to the card (skipped if that sha is already there) → install from the card → reboot.
  Events `fetch.progress {stage, done, size}`.
- Intended app flow: the iOS app fetches the catalog, the user picks, the app sends
  `library.fetch` over BLE; BLE image upload (`install.begin`) is the fallback.
- **Offline switching** (launcher 0.9.14): `library.fetch` installs the requested build
  straight from the card when it's there (no Wi-Fi; reply `fromCard`), and when Wi-Fi, the
  catalog or the download fails it installs the newest version on the card instead (reply
  `offline: true, reason`; never for the launcher). One path for every card install:
  `installFromCard()` (also `install.fromCard` and the picker). The app skips its "no Wi-Fi
  yet" stop when the card has the cartridge and says when Dotty installed an older card copy.
  A failed install screen goes back home after 15 s or a tap (it used to wait for BOOT).
- **Cartridge picker on Dotty** (no phone, no internet): tap the launcher's home screen ("BOOT:
  start Tap: switch") → "Cartridges" list (newest version per cartridge on the card, the
  installed one inverted, 4 rows a page, swipes / the page corner turn pages, back or BOOT
  closes); a row installs from the card and restarts into it, the installed one just starts.
  Get to the launcher with BOOT + PWR. Dev key `c` opens it.

## Power management (`lib/dotty_core/src/power.*`)

- Wake locks (`power::setWakeLock`) = "do not interrupt": while any is held the CPU
  never sleeps. Audio holds one while playing; Wi-Fi/BLE/OTA should add their own.
- Locked + no wake lock + no USB host: touch hibernates, codec/amp power down, audio
  rail off, CPU light-sleeps until the next minute (or PWR). Unlock wakes peripherals.
- Tries not to sleep while a computer is connected over USB (`HWCDC::isPlugged()`), but
  macOS idles the port when no program has it open, so a locked Dotty can still sleep on
  USB: serial goes quiet and BLE is off ("No Dotty found"). Unlock with PWR or reset it
  (serial_read-style RTS pulse) before flashing/BLE tests.
- GPIO17 (power latch) is `gpio_hold_en`'d for life; GPIO6/42/46 are held during sleep.
- Power off on USB power: the latch can't cut power, so it deep-sleeps with PWR (ext0)
  as the wake source; waking is a fresh boot.
- Battery voltage is logged every 10 minutes (`power: battery ... mV`) to measure drain.
- `battery.*` (no fuel gauge, no charge-status pin: STAT only lights the orange LED, and no
  GPIO sees VBUS — checked in the schematic): voltage EMA sampled every 5 s (every wake
  while locked); on battery the shown % only drops when a lower level lasts 1 min (load
  sag: music/Wi-Fi read 40-80 mV low, ~5-10 % in the flat middle of the curve). External
  power = USB host, or the battery voltage jumping ≥ 40 mV between samples with no wake-lock
  change in the last 15 s (on USB, Q5 takes the system off the battery and the charger
  lifts it), or ≥ 4.20 V. Unplugged = a ≥ 40 mV drop, the **load check**, or (backup) the
  voltage not rising ≥ 8 mV in 5 min while below 4.15 V (4.09 V once full). Pulling the cable
  makes the voltage relax over minutes, not in one step (the first version kept "Charging"
  for 5-7 min).
- **Load check** (`loadSag`): CPU spin on core 0 + an async Wi-Fi scan for 100 ms, 256-sample
  ADC reads before/during/after, two cycles averaged. On USB the battery carries no load (Q5),
  so it doesn't dip (measured −3..+3 mV); on battery it does (Wi-Fi alone 3-11 mV, ~8).
  ≥ 5 mV dip = on battery. Runs only while believed on a charger without a USB host (every
  20 s awake, every minute wake locked: the charger pays, at most one check on battery), and
  on battery only when the voltage rises ≥ 15 mV above its low (≤ 2 mV dip = a charger).
  There's no hardware way: no GPIO sees VBUS/VSYS/STAT (Espressif: the S3 USB PHY can't
  detect VBUS); the user doesn't want to solder a VSYS divider onto J1 pins 1/2/6. While charging the % uses the curve
  80 mV lower, capped at 99 until ≥ 4.17 V for 10 min ("Fully charged").
- Lock screen on external power: big battery + bolt + "86%" instead of the padlock,
  bottom line "Charging"/"Fully charged"; ≤ 10 % on battery: "Battery low - charge soon".
  Below 3.40 V for 1 min (or at boot): "Battery empty" screen, then `power::shutdown()`
  (latch off, e-paper keeps it). Info has `battery`, `charging`, `power`.
- Unlocked, the cartridge screens have no battery icon, so the shell shows a power card for
  2.5 s when the power source changes (big battery, %, "Charging"/"Fully charged"/"On
  battery"; a tap dismisses it), then redraws the app. Not for the state at boot, nor for
  changes while locked (the lock screen shows those).
- Measured overnight (locked, light sleep, 8.2 h): 4046 → 3996 mV ≈ 6-7 % ≈ 3.2 mA average,
  ~5 days locked from full. Unplugging from USB drops the reading ~45 mV (charger voltage
  gone), which looked like "90 % → 85 %" but isn't drain.

## iOS app (`ios/`)

- SwiftUI app `ios/Dotty.xcodeproj` (target/scheme `Dotty`, bundle id
  `com.greenspherestudios.dotty`, personal team `DL6N525G5K`, iOS 26.6+, Swift 6). Look and feel = the firefly design
  system in `ios/Dotty/DesignSystem/`, documented in `ios/Design/System/project/README.md`
  (tokens in `tokens.json`; rebuild the JS preview bundle with `python3 ios/Design/System/src/build.py`).
- Build check without signing:
  `xcodebuild -project ios/Dotty.xcodeproj -scheme Dotty -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build`
- Signing uses the user's **personal** team (picked in Xcode); never the company team.
- Structure: `Bluetooth/DottyLink.swift` (@Observable, MainActor; CBCentralManager on the
  main queue with `@preconcurrency` delegate conformances; scan with serial from the
  manufacturer data, `pair` → `remember`, `reconnect` on foreground, pending reconnect
  after any disconnect while paired, `send(cmd, args) async throws` matching replies by
  "cmd", `ensureLauncher`, `waitForReconnect`; `didFailToConnect` retries after 2 s and
  ContentView calls `nudge()` every 10 s while in front: idle → reconnect, "connecting" for
  > 30 s → cancel and start over — a failed attempt used to leave "Searching for Dotty…" until
  the app was reopened), `Bluetooth/DottyProtocol.swift` (UUIDs,
  DottyInfo, DottyMessage, PairedDotty in UserDefaults), `Catalog/Catalog.swift` (GitHub
  catalog, 1-bit icons → pixel-art Image), `Features/Pairing` (scan + pairing sheet),
  `Features/Dashboard` (status, cartridges via `library.fetch`, Wi-Fi via
  `wifi.scan/add/list/remove`), `Features/Music` (the Music cartridge's screen, opened from
  the dashboard: remote, playlists, library; pages Player · Library · Playlists via `LightTabs` (design system); Library has
  search and multi-select (add to a playlist / new playlist / delete); `SongOutbox` = the upload queue, saved in
  Application Support/Outbox with queue.json so it survives closing the app, drops songs
  Dotty already has (stored name + size), keeps the screen awake while syncing, and ends
  an interrupted session with POST /done on the next launch), `Features/Common/DottyBits.swift` (small shared views).
- Connection state on every screen (Dashboard, Cartridges, Wi-Fi, Music, Joke Factory):
  `NotConnectedNotice` shows "Dotty disconnected" (red) for 5 s after a drop
  (`DottyLink.lastDisconnect`), then "Searching for Dotty…" (blue), or "Bluetooth is off";
  it vanishes on reconnect. Controls that need Dotty use `.needsDotty(link)` (disabled +
  dimmed). Exceptions: no notice during a cartridge install or a Music sync (Dotty drops
  the link on purpose), and Music stays enabled while syncing.
- Rows: `LightRow` with an action shows a chevron = opens something. A single choice
  (Weather location, Wi-Fi preferred network) uses `ChoiceRow` instead: no chevron, a
  ring on the right that lights up when chosen (spinner while applying), optional
  trailing controls. Plain actions (e.g. "Update") are buttons in a row, not row taps.
- The dashboard's NavigationStack uses a `NavigationPath` (a typed `[Route]` path silently
  ignored links to pushed screens' own destinations, e.g. a Music playlist).
- Per-cartridge screens: `DashboardView.route(for:)` maps a running cartridge id to its
  screen (only when the firmware has the commands the screen needs).
- `ios/Dotty-Info.plist` (outside the synced folder) adds `UIBackgroundModes:
  bluetooth-central` and ATS `NSAllowsLocalNetworking` (song uploads); the target sets
  `INFOPLIST_KEY_NSLocalNetworkUsageDescription`; merged with the generated Info.plist (`INFOPLIST_FILE`).
- CoreBluetooth doesn't run in the Simulator: test on a real iPhone from Xcode.
- Personal project: no references to the user's employer or its products (the app came
  from a prototype that used a company BLE package, removed on purpose — don't re-add it).
- `ios/LICENSE` is GPL-3.0.

## Long-term goal

An iOS app as Dotty's control panel over a custom BLE GATT command stack: Wi-Fi
provisioning, settings/storage management, firmware updates (BLE OTA), and more.
Design firmware features to be configurable over BLE later.

## Reference

- Waveshare docs: https://docs.waveshare.com/ESP32-S3-ePaper-1.54
- Waveshare code: https://github.com/waveshareteam/ESP32-S3-ePaper-1.54 (clone to a
  scratch dir when needed — examples, schematic, factory firmware)
