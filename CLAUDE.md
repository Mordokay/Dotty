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
- Touch coordinates need a **calibration** (no rotation/mirroring, but not 1:1): x reads ~1.42x
  stretched out from the middle (and ~3 px right), y ~10 px low at the top, both slightly bent;
  the FT6336 clamps its readings to 0..199, so the outer ~25 px on the left and ~33 px on the
  right are **blind strips** (every touch there reads the same; mapped to the strip's middle).
  Waveshare's driver doesn't correct anything. See "Touch calibration" below.
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
  `k` locks / unlocks (read in `shell::update` before the locked branch; while locked it's seen
  only when Dotty is awake: a locked Dotty without a reading program light-sleeps until the next
  minute). Send keys with a short
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
  finger, so scroll by pages; a touch that began and ended between two polls is dropped — it used
  to count as a tap at the previous touch's place, so a swipe down the launcher's cartridge list,
  whose loop re-read the card every pass, opened a cartridge; keep loop passes short), and the lock-screen widget (`shell::Config::drawLockWidget`
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
  the sleeping panda off screen. The bitmap comes from
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
- Screens: lock = clock + padlock icon; power off = the sleeping red panda (`kOffPictures`
  in each main.cpp; the user's portrait was dropped on 2026-10-07 at their request).
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
  (custom GAP handler). A bonded iPhone's first reconnect after BLE comes back on (unlock, boot)
  often fails once (`encryption failed: status 0x7`, remote disconnects) and succeeds ~3 s later;
  so the result screens ("Paired!" / "That didn't work") only show for a pairing that displayed a
  code (`pairingActive`), never for a reconnect (it showed "That didn't work" on every unlock). Once (2026-10-06, after a failed app backup) Dotty refused the bonded
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
  `cartridges/<id>/cartridge.json` = description + requires; `"draft": true` keeps a cartridge
  that's still being built out of the catalog (launchers auto-download every catalog cartridge).
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
    and AP have none → keywords like `site:wsj.com` (headlines only). The Washington Post's
    feeds went 502 on 2026-10-14 (its site doesn't answer either): its sections are now Google
    News searches `site:washingtonpost.com/<section> when:1d` (a feed whose URL is Google News
    is parsed like a keyword, source = the outlet). Check every feed with curl when one fails.
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
  after 50 items (Google's feeds are 130 KB); a topic keeps its 30 best stories ≤ 48 h old (was 10
  until 0.2.5; the pool lives in `stories.tsv` on the card, read into PSRAM), only
  ≤ 24 h when it has ≥ 3 of those (Google ranks by relevance: 40 h old stories topped topics); a
  topic whose feed fails keeps its old stories, but nothing past 48 h is ever shown or carried
  over (a dead feed used to keep its stories for days). Measured: 4 topics, 36 stories in 11 s.
  Dotty fetches by itself (no app needed); the menu's top-left corner (↻) = sync now: a Sync
  screen with "Joining Wi-Fi...", a bar by topics, "Last time: 6 min ago", then "Up to date / 100
  stories" (2 s) or "Some topics failed" + the error (8 s); back leaves it fetching. Dev key `r`.
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
  `r` = the sync corner. App: `Features/News/NewsView.swift` (topics with ★ and a menu; "Add a
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
  `artwork.svg`): **48×48** creatures (drawn ×2 = 96 px on the home and lock screens), each a
  base body + a shared face kit (idle, bob, blink, happy, sad, angry, eat, no), egg (3 frames),
  angel, 16×16 icons as ASCII grids. The base is `tools/pet_art_src/<key>.png` when present (a
  picture made with `docs/pet-art-prompts.md` in an image model: black lines on white, face
  empty; cropped to its ink, scaled to `IMPORT_HEIGHT`, thresholded, stood on the bottom row,
  face at `FACE` (32-px grid coords, tuned per picture); Masko gets white eyes in its mask,
  Chompy its own grin (`SRC_EDITS` flood fills: filled for eating, erased for the unhappy
  poses, then a straight `MOUTH`), Sir Moss no mouth; all 13 pictures are the user's, 2026-10-07).
  `shells()` cuts the egg along its zigzag (per column, the middle of the dark run) into
  `kShell`: bottom, top tipping off (−28° about its lower right corner), top fallen (−100°). The
  hatch animation: crack → top tips → shells on either side of the blinking baby → happy baby.
  Else the first-draft shapes (32-px coordinates drawn
  ×1.5 by `ScaledDraw`). User's choice (2026-10-07): AI pictures for the designs, our face kit
  for the poses (an image model can't keep 8 poses consistent or produce a real pixel grid).
- Animation (unlocked, home screen): a frame every second (`bobFrame`), per state: awake and fine
  = bob + wander every 4th frame; asleep = breathes (2 px) + Zzz drifts up-right; sick = shivers
  (±2 px) + skull bobs; a meter empty = sighs (3 px); discipline call = cross face / looking away;
  egg wobbles; poop steams; the angel floats; paused = still. Asleep it animates every 2 s (a
  calmer breath; the dark screen ghosts most). Animation frames use `shell::refresh(false, false)`
  with a full refresh every 2 minutes with the light on, every minute in the dark (the shell's
  every-30 full refresh would flash every 30 s), and
  an animation ending in the dark gets a full refresh (the user saw the speech bubble's ghost on
  black); locked stays once a minute.
- Refusals (play, food, medicine, clean, scold) play in the room as it is (dark with the light
  off; asleep it keeps sleeping, with its Zzz) and say why in a speech bubble: "Shh... sleeping",
  "Too sick to play/eat", "I'm full!", "I'm not sick!", "Already clean!", "I did nothing!", "No!".
- Dotty: home = pet, poop 2×2, top bar Food ·
  Light · Game · Medicine, bottom Clean · Meter · Discipline · bell (dark while calling). Screens:
  Food (meal/snack, bites), Light, **Games** (a 2×3 menu: Left/Right, High/Low, Shells, Memory,
  Clap!, Surprise; every game is 5 rounds with a dot per round and ends with `pet::finishGame` —
  3+ wins = +1 happy, −1 weight — so the 1996 rules stay; only how a round is won differs):
  Left/Right = the P1's (decided at random before you choose, per-character odds); High/Low =
  the P2's (1–9, the number carries over); Shells = the pet under one of 3 cups, 2–5 swaps shown
  one per 1.1 s as a curved arrow between two cups (e-paper can't move them), tap the cup;
  Memory = 4 quarters with icons + notes (C E G C), sequences of 3–7 shown 0.75 s each, repeat by
  tapping; Clap! = Dotty beeps a rhythm (2–6 beats, gaps 0.5/1/1.5 s) and the microphone times
  your claps (`player.startCapture(30)`, 10 ms blocks; the room's level over 0.3 s ×4, min 4000,
  is the clap threshold; 150 ms between claps; done 1.8 s after the last): the count must match
  and every gap be within ±35 % (min ±0.25 s); beats and claps drawn as two time-spaced rows. Meter (age, weight, discipline bar, hearts, Pause + Sound buttons), animations for eat,
  refuse, medicine, clean (duck), scold, hatch, evolve, game end; death = angel + age + cause, tap
  for a new egg. Lock screen (`Config::lockScreen`, steps the engine each minute wake), the user's
  layout: top half = the pet as the home screen shows it (×2; Zzz ×2 by its head when asleep,
  skull ×2 when sick, up to 4
  poops beside it, the corner dark with the light off; icons with an edge in the background colour;
  a small padlock top-left — only Lumo's ear reaches there) | clock + battery, name, "Age 3  12 g", then alert icons ×2 (call,
  skull, poop, bulb, Zzz, pause) or one word ("Hungry!", "Bored!", "All good"); bottom half =
  three big rows: hunger hearts, happy hearts, discipline as one continuous bar (as on the
  Meter screen), icons ×2.
- It lives only while this cartridge runs: on boot `lastMinute = now` (power off / another
  cartridge = paused). Saved as raw `State` in `data/pet.bin` (magic + size; bump
  `kStateVersion` when the struct changes) on events/actions and every 10 min;
  `data/history.tsv` per pet that died.
- Sound: `AudioPlayer::playNotes` (dotty_core) synthesises square-wave tunes with a plucked
  envelope (no files); tunes in main.cpp (call chirp, eat, happy, win/lose, evolve, hatch, poop,
  sick, medicine, clean, scold, no, death, and a 14 ms click on every tap). One chirp per call, never repeated;
  locked + asleep: the codec is powered up for the chirp and down again. Three kinds, each with
  a switch: clicks (taps), actions (what you do), alerts (what the pet does by itself); `sound`
  mutes all (Meter screen or app); quiet hours mute only alerts (you're touching Dotty for the
  others). NVS `pet`: sound, clicks, actions, alerts, volume, quietFrom/quietTo (minutes;
  default 23:30–08:00 so bedtime calls still sound).
- BLE: `pet.status` (species, stage, generation, age, weight, hunger, happy, discipline %,
  mistakes, sick, poops, asleep, lightsOn, paused, calling, needs, died?, sound{}),
  `pet.pause {on}`, `pet.sound {on?, clicks?, actions?, alerts?, volume?, quietFrom?, quietTo?}`, `pet.history`,
  `pet.newEgg {force?}`; event `pet.changed`. App: `Features/Pet/PetView.swift`.
- Icons: the thin outline ones (`BOLD_ICONS`: bell, syringe, duck, Zzz, empty heart, meter) get
  2-px strokes, the bulb only vertically (`BOLD_X_ICONS`, lighter); the discipline triangle
  (a 2-px "!") and the Game icon (a ▶ in a ring: "play", also the happy row's icon) are drawn bold
  in their grids; detailed ones keep 1 px (thickened, the fork and candy blobbed).
- Dev keys: `F` fast-forward (~20 min/s, saved when stopped), `E` evolve now, `q w e r` / `z x c
  v` = the eight functions, in the game menu `a b c e f` pick a tile (`d` is the logger's
  replay key), `C` = a clap while listening (`p` is the shell's pairing preview, `k` lock).
  Rounds are logged ("Memory round 1: lost").

## Facts cartridge (`cartridges/facts/`)

- 10,136 dinner-table facts (user: "cool things people normally don't know", not nerdy; each fits
  one screen, two at most: <= 200 characters, mostly 80-180) in 20 categories, **built into the
  firmware**: `facts.txt` ("@key|Name" before each category, then a fact per line, ASCII) is
  embedded with `board_build.embed_txtfiles` (symbols `_binary_cartridges_facts_facts_txt_start/
  _end`; ~1.1 MB, the image ~2.7 MB of the 4 MB slot), so it never downloads or updates. Made on
  2026-10-15 by 20 writer subagents (one per category, ~540 each, rules: true, no myths, dated
  records, plain words) + 10 reviewer subagents (fix or replace false/dubious/dull lines; they
  found many myths and stale records) + a duplicate pass (word-overlap pairs across categories,
  down to 0.30 similarity: 736 repeats removed, each kept once where it fits best: 10,872 →
  10,136, 465-537 a category);
  `tools/build_facts.py SOURCE_DIR` merges one file per category into facts.txt (checks ASCII,
  40-200 chars, sentence ending; drops duplicates and near-duplicates across categories; the
  category order and names live there). Edit facts.txt directly for small fixes.
- `facts_store.*`: indexes the text at boot into PSRAM (pointers into flash), FNV-1a of a fact's
  text = its id. Card `data/seen.bin` (magic + hash of facts.txt + a bit per fact; reset when
  facts.txt changes), `data/favourites.txt` (ids). `next(scope)` = a random unseen fact of a
  category / all (`kAny`); a scope seen in full starts over; favourites go in starring order.
- Dotty: menu = nav "Facts" + page; page 1 starts with a row of three buttons (56 px: icon over
  a 9pt label - Random (shuffle icon) = browse every category, Play (▶) = the same as a shuffled
  playlist that moves on by itself, Saved (★) = favourites), then categories with how many are
  still new (38 px rows, 2 on page 1, 4 after). Fact screen: category in the nav, ☆/★ right
  corner, the text centred (scrolls with a scrollbar when long); tap / swipe left / BOOT = next,
  swipe right = back (history of 50), long press = play / stop playing the current scope.
  Playing: ▶ before the title (‖ paused), tap = pause/resume, a fact stays 8 s + 70 ms a
  character, `shell::wake()` meanwhile, stops after 30 min untouched. Lock widget: a fact that
  fits 4 lines (tried from a mixed per-5-minute-slot start). Dev keys: `P` play, `R` random, `n`.
- BLE: `facts.status` {count, seen, favourites, categories[{key, name, count, seen}], showing?},
  `facts.favourites` {facts[{id, text, category, favourite}]} (newest first), `facts.favourite
  {id, on}`, `facts.next {category?}` (shows one on Dotty); event `facts.changed`. App:
  `Features/Facts/FactsView.swift` (on Dotty now, "Show a new fact", categories → a fact from
  one, favourites with share / remove).

## Companion cartridge (`cartridges/companion/`)

- Talk to an AI: hold BOOT, speak, it answers out loud with the text on screen (spec/plan:
  `docs/superpowers/{specs,plans}/2026-10-22-companion-talking*`; roadmap and later pieces —
  memories next, then a Bluetooth-through-the-iPhone spike, roles, scheduled moments — in the
  memory file `dotty-ai-companion`). English only.
- Engine: **Gemini Live API** (`models/gemini-3.8-live`, WebSocket
  `wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=…`),
  our own client: `ws_frame.*` (RFC 6455 frames, handshake, own SHA-1; host-tested), `ws_client.*`
  (esp_tls + cert bundle; a send that can't finish drops the connection; `close()` sends a close
  frame first — without it Google kept the session open and a reconnect seconds later was refused
  "exceeded your current quota"), `live_protocol.*` (setup, push-to-talk `activityStart/End` with
  automatic activity detection off, transcription both ways, `set_mood` tool NON_BLOCKING +
  SILENT, resumption handles; errors arrive as a close code + reason), `gemini_live.*` (session).
  `set_mood` comes in its own turn before the speech turn; it's answered **once per question**
  (each answer made Gemini speak again: a loop that billed 23,792 tokens for one question) and 3
  turns nobody asked for close the session (`turn_tracker.h`, host-tested: also drops an
  interrupted reply's leftovers, which used to become a phantom turn). Google's goAway (~10 min)
  closes the session after the reply and the next press resumes with its handle (the inline
  reconnect froze the screen; still untested on the device). TCP keepalive + 15 s of silence while
  a reply is due = "Connection lost". The system instruction says short replies, always
  English (a silent turn once got Spanish), the local time and the Weather cartridge's place.
- Measured: Wi-Fi join ~4-5 s, TLS 1.3-2 s at home (8.4 s on an office network), first reply audio
  1.0-1.8 s after release (the set_mood turn costs ~0.5 s). Free tier: ~1,800 tokens per short
  exchange (each turn re-reads the session).
- **Bluetooth is off during a session**: with it on (iPhone connected) TLS left 36 KB internal RAM,
  largest block 12 KB, and a session hung in its sends until the watchdog reset Dotty. So a session
  opens on the **first BOOT press** (not on unlock: the shell restarts BLE after the wake hook, and
  the app could never reach Dotty), closes after 3 idle minutes, on lock or when settings change.
  **Talking while it connects**: the voice goes into a PSRAM buffer (≤ 60 s) and is sent once the
  session is ready, a few chunks per loop pass (sending 7 s at once overflowed the 2 s capture
  buffer); you never wait to speak, only for the reply. Locking while it connects only flags a cancel: the connect
  task closes what it opened (closing its socket from the loop was a use-after-free).
- Audio: `voice.*` (capture task on core 0 → 64 KB PSRAM stream buffer, 100 ms chunks, 16 kHz;
  replies 24 kHz into a fixed 60 s PSRAM `PcmRing` backlog (a growing string doubled to 8 MB on a
  long reply), then `AudioPlayer::startStream/writeStream/endStream` — dotty_core: a 2 s PSRAM
  `PcmRing`, playback starts with 250 ms buffered). Reply text is paced by the audio heard (a piece
  shows once the speaker has played the audio that came with it) and shown **a page at a time**
  (3 lines; the next line clears the page — the user found scrolling text hard to read); the reply
  stays after it ends, swipes = pages ("2/3").
- Screens: avatar 96 px (48 px beside the text when text-only), gear (top left) = settings: volume
  − / + (beep), Speak out loud / text only, Voice ‹ › through the **favourite voices** (NVS
  `companion/favourites`, default "Zephyr,Achernar", starred in the app), playing each one's sample
  (`voices.bin`: 30 MP3s, 32 kHz MPEG-1 — 24 kHz MPEG-2 ones gave "No MP3 frames" — packed by
  `tools/build_voice_pack.py`, embedded, copied to `data/voices/` through a RAM buffer: a direct
  write from flash left zeros). Clock (top right) = today's chats. Lock screen: the avatar asleep
  under the clock + "N chats today".
- Quota: a close saying quota without a retry time waits 1 minute first (short limits look the
  same); a second in a row = "I'm out of words for today. Back at 08:00." (midnight Pacific in
  local time, `nextPacificMidnightUtc`). The free tier has no quota API; usage is counted on Dotty.
- Card `data/history/YYYY-MM-DD.jsonl` (one line per exchange: you, dotty, mood, ms, tokens), NVS
  `companion`: key, voice, favourites, avatar, textOnly, volume, usage counters, outUntil.
- Avatars: 10 (robot, dog, cat, fox, owl, bear, girl, guy, ghost, dragon) × 12 faces at 96 and 48
  px, generated by `tools/avatar_art.py` from the user's pictures in `tools/avatar_art_src/` (prompts:
  `docs/companion-avatar-prompts.md`), faces placed per picture (`FACE`, `MOUTH_DROP`, `ERASE`);
  also the app's previews (`ios/Dotty/Resources/Avatars`) and the install icon.
- BLE (only while no session): `companion.status` {connected, state, mood, avatar, voice,
  favourites, volume, textOnly, keySet, keyHint (last 4), today{turns, tokens}, outUntil?},
  `companion.settings {avatar?, voice?, favourites?, volume?, textOnly?, key?}` (key trimmed, never
  logged or sent back in full), `companion.history {day?}`, `companion.forget {day?}`; event
  `companion.changed`. App: `Features/Companion/` (CompanionView, VoicesView with ▶ and ☆,
  CompanionHistoryView).
- **Memories** (0.2.0; spec/plan `docs/superpowers/{specs,plans}/2026-10-22-companion-memories*`):
  `memory_store.*` (plain C++, host-tested): ≤ 300 memories of ≤ 200 characters (code points, cut
  at a word) in groups you / people / likes / plans / other, no duplicates (case and spaces aside),
  ids never reused; `memories.*` keeps them in `data/memory/memories.json` (via `.part`; a broken
  file → `memories.bad.json`, empty store). Every memory goes into the system instruction at
  connect, grouped and numbered (~25 tokens each, every turn). Tools `remember {text, group}`,
  `update_memory {id, text, group?}`, `forget_memory {id}` (NON_BLOCKING, answered once each —
  measured: no extra turns; `remember`'s answer carries `memoryId`, since the model guessed ids of
  memories saved in the same chat). Applied only inside a question (TurnTracker). Note under the
  avatar for 3 s: bookmark + Remembered / Updated / Forgotten / Memory full; history lines get
  `memory`. **End-of-chat review**: at the 3-minute idle close a silent text turn asks Gemini to save
  what it missed (it had kept "likes popcorn" but not "my name is Peter"), ≤ 20 s, then closes; not
  on lock. App: Companion › Memories (`MemoriesView`: groups, search, edit sheet, swipe to delete,
  add, forget everything); BLE `companion.memories {offset, limit ≤ 50}` (newest first) →
  {memories, total, max}, `companion.memory.add/update/delete/clear`, `status.memories`; every memory
  command answers `{ok: false, error: "unavailable"}` without a card.
- **Roles** (0.3.0; spec/plan `docs/superpowers/{specs,plans}/2026-10-22-companion-roles*`): up to
  8 companions, each its own person: `companion_store.*` (host-tested: Profile — name ≤ 24,
  personality ≤ 600, traits 6 × 40, quirks 6 × 60, way of speaking ≤ 300, backstory ≤ 600, voice,
  avatar or `photo`; CompanionList — ids `coach friend partner teacher debug` / `c<N>` never reused,
  never the last one; `migrationPlan`), `companions.*` (card `data/companions/<id>/{profile.json,
  memory/, history/, photo.bin}`, `index.json`, NVS `companion/active`; first boot of 0.3.0 moves
  piece 2a's `data/memory` + `data/history` into Debug's folder, resumable). Presets: Coach (dog,
  Puck), Friend (cat, Zephyr), Partner = the user's girlfriend (girl, Achernar), Teacher (owl,
  Charon), Debug (robot, named, holds the old memories). **Waking up**: a nameless companion's
  persona (`persona.*`) says it just woke up and can't remember who it is — "speak softly and
  hesitantly, a little lost and sad" (a listening A/B on the device: slower, lower, doubtful) — and
  `set_name {name}` saves the name it's given; until then "Not awake yet. Hold BOOT". Every persona
  ends with **acting notes** (commit to each line's emotion: whispers, laughs, sighs…; the user's
  A/B: a little spark, +~200 tokens a turn, kept) and set_mood has 16 moods on the 12 faces.
  Affective dialog is not supported on gemini-3.8-live (1007 both ways). (The acting demo's A/B
  screen and its `data/act_*.wav` were removed for the replay; boot deletes leftover files.)
  Switching companion (settings row, app pager, `companion.select`) closes the session; memory and
  name calls only land on the session's companion. Layout: avatar 128 px, text 2 lines, settings 4
  rows (Companion, Volume, Speak, Voice); a photo avatar shows a mood badge. **Keys**: up to 3, one
  per session in turn (`key_ring.h`, host-tested), a key out of allowance skipped until its reset,
  usage per key; the app checks each key with Google (`GET /v1beta/models`) before keeping it
  (Keychain + Dotty). BLE: `companion.list/get/select/create/update/field` (long fields in pieces,
  `piece_buffer.h`) `/delete/photo/photo.clear`; memory and history commands take `companion`.
  App: `CompanionView` pager (a page per companion + a "+" page), `CompanionSettingsSheet` (gear:
  keys, Speak, volume, today), `AvatarPhotoEditor` (circle crop, dithered 128/48), the **Gemini
  co-designer** (`GeminiText`, `gemini-flash-lite-latest`: ~2-9 s, where 3.8 Flash took 20-60 s
  thinking) with ✨ Suggest per field. Dev keys S (next companion), W (forget its name), P (test
  photo / back), Z ("Zoë 🌙" / Luna), I (every profile into the log), U (a typed turn: "you've
  become an ocean lover…"), G (a typed "a whole minute of surf talk"), E (end the chat now: the
  review runs), A (replay play / pause; serial `4` taps the loop button).
- **Replay** (user, 2026-10-23): each reply's audio is copied as it arrives (PSRAM, ≤ 60 s, freed
  after) and, once heard, saved as the companion's `last.wav` (24 kHz) + `last.json` (`last_line.*`,
  host-tested: text pieces timed in ms to the audio, so a replay pages the text like the first time;
  WAV header; `Replay` play / pause / loop, a loop stops after 30 min untouched). Buttons in a
  column under the gear (▶/‖, loop dark when on; the bottom third stays for text and meter), a
  progress bar between the avatar and the text while it plays; BOOT, settings, lock or a switch
  stop it. BLE `companion.replay {action?: play|pause|toggle|stop, loop?}` → {last?: {text,
  seconds, t, mood}, replay: {state, loop, available}} (also in `companion.status`), event
  `companion.replay`. App: `LastLineCard` on the active companion's page (play/pause, loop).
  Saving a 30 s line takes ~1 s after it has played. **Long replies**: asked for "one whole
  minute" (also "about 150 words", and with an instruction rule to honour lengths) Gemini Live
  gave 25-30 s every time — a single spoken reply stays around half a minute.
- The lock screen's "N chats" = this companion's talks today (`conversations()` over its history:
  turns within the 3-minute idle close are one talk); it was the day's exchanges across every
  companion (79 on a test-heavy day).
- **Evolution** (user, 2026-10-23: companions aren't set in stone; before any release): `evolve
  {field, value, why}` (personality, traits, quirks, speaking, backstory; the whole new field, a line
  per item/sentence) lands **once per chat** — the model calls it mid-chat when told it changed (the
  first device run: called during the turn, so "review only" ignored it), else in the end-of-chat
  review (`reviewPrompt`: memories first, "most chats change nothing", the fields listed with
  "(pinned)" marks, third person). `applyEvolution` (host-tested) strips "- " marks and "(pinned)",
  puts back pinned lines it dropped at their old place, and over a limit drops the last unpinned
  lines. **Pins per line**: the profile keeps `pins: {field: [line text]}`; over BLE they're line
  numbers (`companion.pins {id, field, lines}`, `companion.get` → `pins: {field: [n]}`) since a pinned
  sentence can pass 512 bytes. Log: `evolution.jsonl` per companion ({t, field, before, after, why},
  an undo adds `undo: t`), `companion.evolution {id, limit = 20}` (newest first, `undone`),
  `companion.evolution.undo {id, t}` (puts `before` back). App: every character field is edited line
  by line (`LinesField`: pin at the left, ✕, "Add line"; prose split into sentences), Suggest keeps
  pinned lines, and "How {name} has changed" (`EvolutionView`: why, − / + lines, Undo). Device run:
  Debug's way of speaking became surf slang (left for the user to try Undo).
- Google refuses a new session within ~40 s of the previous one ("exceeded your current quota"),
  even after a proper close: the 1-minute wait handles it. Bluetooth comes back 1 s after a
  session's Wi-Fi is off (one BT-controller interrupt-watchdog crash, in ROM
  `ll_connection_update_ind_handler`, came right after it came back at once). Weak Wi-Fi
  (< -75 dBm) shows "Weak Wi-Fi: replies may stutter"; streamed audio refills a second after
  running dry (dotty_core `stream_gate.h`, host-tested: -78 dBm gave 1,137 gaps in one reply).
  Each reply logs "spoken; longest loop pause N ms" (choppy = network if small).
- The key: NVS `companion/key` from the app; `cartridges/companion/dev_key.h` (gitignored, never
  commit) seeds an empty key on the bench. Dev keys: `r` simulated BOOT hold, `t` gear, `X` close the
  socket, `m` cycle moods, `q` fake quota close, `Q` clear it, `J` send invalid JSON (the server
  closes: tests the close path), `M` break the memory file, `N` show the memory note. Host tests:
  `python3 tools/companion_test/run.py` (frames, protocol with captured samples, Pacific reset,
  pages, voices, voice pack, PCM ring, turn tracker, memory store, stream gate, companion store,
  persona, pieces, key ring).

## SD library + Wi-Fi fetch (launcher)

- `cartridges/launcher/library.*`: cartridges on the card as `/cartridges/<id>/firmware/<version>`
  `.bin/.json/.icon`. Every BLE install is copied there (verified). `install.fromCard
  {id, version?, sha256?}` flashes from the card in ~2.5 s (whole region erased up front;
  4 KB sequential erases took 10 s). `library.list` lists the card.
- `lib/dotty_core/src/net.*` (every firmware): up to 8 saved networks as JSON in NVS
  namespace `wifi`; `net::connect()` scans and joins the strongest known one, and the strongest
  access point of it (`WIFI_ALL_CHANNEL_SCAN` + `WIFI_CONNECT_AP_BY_SIGNAL`: on the user's mesh the
  default first-found node gave -86 dBm where the near one gave -40). HTTPS via
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
- **New cartridges arrive by themselves** (launcher 0.9.28): the daily check also downloads every
  catalog cartridge that isn't on the card (Facts never appeared in the picker: it had been
  USB-flashed, and the check only refreshed what was already there), except ones the user removed:
  `storage::removedByUser` (NVS `storage/removed`, ",id,id,", set by `storage.remove` in every
  firmware, cleared by installFromCard / library.fetch / a BLE install). A USB-flashed cartridge
  gets a copy on the card 8 s after the launcher starts (`library::saveSlot`, size and SHA-256
  worked out from the image), and the next check gives it the catalog's icon (`saveIcon`).
- **New cartridges arrive by themselves** (launcher 0.9.28): the daily check also downloads every
  catalog cartridge that isn't on the card (Facts never appeared in the picker: it had been
  USB-flashed, and the check only refreshed what was already there), except ones the user removed:
  `storage::removedByUser` (NVS `storage/removed`, ",id,id,", set by `storage.remove` in every
  firmware, cleared by installFromCard / library.fetch / a BLE install). A USB-flashed cartridge
  gets a copy on the card 8 s after the launcher starts (`library::saveSlot`, size and SHA-256
  worked out from the image), and the next check gives it the catalog's icon (`saveIcon`).
  Tested 2026-10-17: Facts 0.1.0 came down on a hotel guest Wi-Fi and showed in the picker.
- **Cartridge updates without the phone** (launcher 0.9.23, `updates.*`): a background task
  (core 0, 12 KB, Network wake lock, also while locked) reads the catalog once a day (NVS
  `updates/checked`, local epoch; 1 h after a failed check; the first one 15 s after the launcher
  starts) and downloads a newer version of every cartridge already on the card, and of the
  launcher, into `firmware/` (`.part`, size + SHA-256, `library::commit`), then keeps the newest
  2 versions per cartridge (never prunes the launcher: Rescue may need its `good` one). Nothing is
  flashed in the background: `startCartridge()` (BOOT, the picker's installed row, `launcher.start`)
  installs a newer card copy first (`installFromCard`, ~2.5 s, then restarts; a failed one is
  skipped next time), home says "News: update ready", the picker's installed row "new 0.2.7".
  Third bordered button (arrow into a tray, a dot while busy) → Updates screen: progress
  ("Downloading", the cartridge, a bar, "2 of 7"), then "7 updates ready / They install on start"
  or "All up to date / Checked 5 min ago" or "Couldn't check"; "Update Dotty / to
  0.9.26" (dark, two lines: one line was cut off) when the card has a newer launcher → `installFromCard` → Rescue; "Check now"
  (opening the screen checks too, unless one ran < 5 min ago). Anything else that needs Wi-Fi or
  restarts stops it first (`updates::stop(keepWifi)`: library.fetch keeps the connection,
  installs / Wi-Fi setup / factory reset don't; a transfer session stops it from loop()). First
  run: 7 cartridges, ~1.5 MB each, in 100 s at −48 dBm. Each download is tried 3 times (5 s
  apart; at −89 dBm the launcher's broke off after 80 s). The screen says "Download failed" when
  the catalog was read, and network errors in words ("The Wi-Fi let it down…"). Once (2026-10-14,
  after that failure, BLE connected) every HTTPS open failed at once for minutes, even at −42 dBm,
  until a restart: `net` now logs the esp_err and the internal heap when an open fails. Cause
  found with it: ESP_ERR_HTTP_CONNECT with 49.6 KB internal free, largest block 31.7 KB — BLE
  connected plus the parsed catalog (~15 KB, internal: allocations < 16 KB stay internal) left
  mbedTLS too little. 0.9.25 reads and parses the catalog in PSRAM (`PsramAllocator` for
  ArduinoJson, a 64 KB PSRAM buffer) and frees it before the first download; the log says the
  heap before downloading. Keep big buffers out of internal RAM in anything that opens HTTPS
  with BLE up. 0.9.26: no auto-lock while a check runs (`shell::wake()` each pass while
  unlocked; one that started locked stays locked). Dev key `u`. The loop's order matters:
  open screens (Updates, calibration, Wi-Fi setup, picker) take their taps before the home
  buttons' hit tests (0.9.22 opened calibration from a tap on the Wi-Fi password field).
- **Cartridge picker on Dotty** (no phone, no internet): tap the launcher's home screen ("BOOT:
  start Tap: switch") → "Cartridges" list (newest version per cartridge on the card, the
  installed one inverted, 4 rows a page, swipes / the page corner turn pages, back or BOOT
  closes); a row installs from the card and restarts into it, the installed one just starts.
  The list is read from the card once when it opens (`pickerEntries`), not every loop pass.
  Get to the launcher with BOOT + PWR. Dev key `c` opens it.
- **Touch calibration** (launcher 0.9.21, `touch_calibration.*`; correction in dotty_core
  `touch.*`, every firmware): `Touch::Calibration` = screen x and y each a quadratic in both raw
  coordinates (u, v = (raw − 100) / 100; terms 1, u, v, uv, u², v²); a straight line per axis
  stayed ~3 px off. Saved in NVS `touch` (`magic` + `cal` blob; erased by a factory reset); the
  defaults are the prototype's own (2026-10-14). Screen: rounds of 15 crosses (x 36-160 — the
  user tried them further out: at 166, next to the right strip, taps that fell right read as
  clamped and were dropped, which bent the fit; y 66-186), any order, each tap matched to the
  nearest cross (≤ 35 px). After each round: least squares over every tap so far (a tilted plane
  for rounds 1-2, then the quadratic), used at once (next round's dots land closer), and a
  "Calibration" bar (user: no px, no jargon): RMS of the per-cross average errors, × m/(m−k)
  for the fitted terms; 100 % at ≤ 2.5 px, 0 % at 7 — fills as rounds average the fingertip's
  ~4 px scatter out (3-5 rounds). Again / Done (dark = suggested). Done saves, then a **typing
  test** on the real keyboard: "dotty", "quiz", "pumpkin" (both edge columns), wrong letters
  underlined, slips counted, summary "3 words, 1 slip" with Again / Done. Opened by: the welcome
  flow's first step (`Setup::Touch` while `!touch.calibrated()`; "Skip" top right or BOOT keeps
  the defaults), the crosshair button under the Wi-Fi one on the launcher home (0.9.22: both in
  36 px boxes with a 5 px corner radius, 5 px apart, at the left edge), a long press there, or
  the app's Dashboard › Settings › Calibrate touch (`touch.calibrate`, after ensureLauncher).
  Every calibration tap is appended to the card's `/logs/touch.tsv` (epoch, round, cross, raw,
  landed) — the log ring only keeps a few rounds. Dev keys: `t` opens it, `T` jumps to the
  typing test. The **keyboard** (`keyboard.*`, shared with Wi-Fi) takes its first/last column
  edges from the blind strips (`Touch::bands`): on the prototype the right column starts at
  166 (a tap on the right of a 25 px "u" read as "i"), the others share the rest.
- **Wi-Fi on Dotty** (launcher 0.9.19, `wifi_setup.*`, no phone needed): the Wi-Fi fan in the
  home screen's top-left corner (0.9.20; taps within 60 px of the corner; on the welcome screens
  too; 0.9.19 had it as the picker's last row), or any tap on the "pick a Wi-Fi" welcome screen,
  dev key `i` from home →
  "Looking for networks" (`net::scan`, blocking, once the screen is up) → list (38 px rows: ✓
  saved, padlock, 4 signal bars; last row "Look again") → keyboard for secured networks → "Joining..."
  (`net::connectTo`, then `remember` only if it worked, like `wifi.add`) → "Connected! / Naru is
  saved" 3 s, back home; or "Couldn't join" 6 s / tap, back to the keyboard with the
  password kept (< 8 characters is refused before trying). **Keyboard** (user's design rounds:
  10x6 of 20 px → iPhone-style pages → 8 columns → 4 rows): 8 x 4 keys of 25 x 31 px under the
  nav bar (back · network · "Join" in the right corner, like iOS) and the password field (tap
  it = show/hide; masked with `*`, the last typed character visible until the next key; the end
  of a long one shown). Letters page = QWERTY with each row's overflow moved down to the right:
  `qwertyui / asdfghop / zxcvbjkl / ⇧ n m 123 space ⌫`; `123` = digits + common punctuation,
  `#+=` = brackets, maths, the rare ones (every printable ASCII has a key). Shift one-shot, twice
  within 0.8 s = caps lock (bar under the arrow). Taps during a partial refresh are kept and drawn
  together. Dev keys on the keyboard: letters type, `^` shift, `#` next page, `=` join; `n` on
  the list picks the first network. The keyboard itself lives in `keyboard.*` (see Touch
  calibration).

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
- Battery voltage is logged every 10 minutes (`power: battery ... mV`) to measure drain, and
  appended to the SD card's `/logs/battery.tsv` (time, epoch, mV, smoothed, % shown, external, USB,
  locked, cartridge, event: boot / off / empty) by every firmware, to fit the % curve to this
  battery from a real discharge (2026-10-12: the generic curve showed 4-5 % at 3.57-3.61 V and Dotty
  still ran over a day; ~6 mV/h locked). Serial `B` prints the file between `#BATTERY <size>` and
  `#END`. **Fitted** (2026-10-20, `battery.cpp` `curve`): the 400 mAh battery ran 4.09 -> 3.48 V in
  57 h nearly always locked (~6 mA average); the share of time left at each voltage gave 3.59 V =
  19 %, 3.72 V = 36 %, 3.81 V = 45 % (the generic curve said 4 %, 18 %, 44 %). The same day the
  user fitted a **1000 mAh** battery (expect ~2.5x the runtime, ~6 days locked): check its first
  discharge in the log against the curve.
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
