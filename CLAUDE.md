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
~/.platformio/penv/bin/pio run -e launcher -t upload --upload-port /dev/cu.usbmodem1101  # factory, boots launcher
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
  the nav bar's left corner, `3` its right corner, `5` the middle); `]` / `[` swipe left / right. Send keys with a short
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
  (music, weather, …), never mixed; the iOS app swaps them over BLE. A permanent
  launcher in the `factory` partition installs cartridges and shows progress with the
  cartridge's icon. Shared code lives in `lib/dotty_core/src/`; each cartridge is
  `cartridges/<name>/` with its own `[env:<name>]` in `platformio.ini`
  (`build_src_filter = +<name>/`). Never add cartridge-specific code to dotty_core
  unless a second cartridge needs it.
- Flash layout (`partitions.csv`): `factory` 2 MB = launcher (grew from 1.5 MB when
  Wi-Fi + HTTPS pushed it to 97 %), `ota_0` 4 MB = the active cartridge, `storage`
  1.9 MB LittleFS. Cartridge envs upload to ota_0 with
  `boot_app0.bin` (boots the cartridge); `tools/pio_launcher.py` makes the launcher env
  upload to `factory` with a blank otadata (boots the launcher). The platform resets
  `ESP32_APP_OFFSET` to ota_0 during the build, hence the pre-actions in that script.
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

- **Partition table is OTA-ready from day one** (`partitions.csv`: two 3 MB app slots,
  1.9 MB `spiffs`/LittleFS, coredump). The long-term goal is firmware updates over BLE
  from an iOS app; don't switch to a no-OTA layout.
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

- NimBLE-Arduino 2.x + ArduinoJson 7. Device name `Dotty-XXXX` (MAC suffix), the same
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
  bleak/macOS also gets a pairing prompt on the first command now.
- Core commands: `core.ping`, `core.info`, `core.forget`, `core.toLauncher` (cartridges only),
  `core.time {local}` (registered by the shell: local epoch seconds; sets the RTC when ≥ 2 s
  off, replies `drift`; the app sends it on every connection — `DottyLink.syncClock`;
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
- **Launcher updates from the app** ("Dotty system" panel on Cartridges): the launcher is in
  the catalog with `"system": true`. Any launcher fetches it like a cartridge
  (`library.fetch {id: launcher}`) into ota_0 and boots it; the new launcher, seeing it runs
  from ota_0, copies its image into factory (`installSelfIfUpdate`, before the shell: no BLE
  from the slot copy), verifies it (`esp_image_verify`), sets factory as boot and restarts;
  the factory launcher finds "launcher" in ota_0 and erases it. Power cut midway → the ota_0
  copy boots again and redoes it. The app then reinstalls the previous cartridge with
  `install.fromCard`. Info has `launcher` (factory version) in every cartridge, so the app
  can offer the update anywhere; apps hide `system` entries from the cartridge list.
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
  "cmd", `ensureLauncher`, `waitForReconnect`), `Bluetooth/DottyProtocol.swift` (UUIDs,
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
