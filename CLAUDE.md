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
- **No IMU** (no shake detection). Use SHTC3 for real temperature/humidity instead.
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
- Buttons are active-low with pull-ups. The PWR button is still held down right after a
  battery power-on, so ignore it until it has been released once.
- Battery: `analogReadMilliVolts(4) * 2`.
- RTC PCF85063 at I2C 0x51 holds local time. Until Wi-Fi/BLE time sync exists, firmware
  sets it to the build time (`__DATE__`/`__TIME__`) when it is invalid or older.
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
- Flash layout (`partitions.csv`): `factory` 1.5 MB = launcher, `ota_0` 4.75 MB = the
  active cartridge, `storage` 1.6 MB LittleFS. Cartridge envs upload to ota_0 with
  `boot_app0.bin` (boots the cartridge); `tools/pio_launcher.py` makes the launcher env
  upload to `factory` with a blank otadata (boots the launcher). The platform resets
  `ESP32_APP_OFFSET` to ota_0 during the build, hence the pre-actions in that script.
- Every firmware declares `DOTTY_CARTRIDGE(id, name, version)`; the struct lands in
  `.rodata_custom_desc` at offset 0x120 of the image, where the launcher reads it
  (`cartridge::readInstalled`). `cartridge::rebootToLauncher()` / `startInstalled()`
  switch boot partitions.
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
  JSON, refreshed every 10 s), `…0002` Command (write, JSON `{"cmd": …}`), `…0003` Event
  (notify, JSON replies `{"cmd": …, "ok": …}`), `…0004` Data (write-no-response, for
  installs).
- Register commands with `ble::on("<cartridge>.<verb>", handler)`; handlers run on the
  main loop via `ble::poll()` (called by `shell::update`), never on the BLE task. A
  handler that reboots must defer it until after its reply has been notified.
- Core commands: `core.ping`, `core.info`, `core.toLauncher` (cartridges only);
  launcher: `launcher.start`, `install.begin/end/abort`; music: `music.status`,
  `music.toggle`, `music.volume`.
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
- **Catalog** (`tools/build_catalog.py`): builds every cartridge except the launcher into
  `dist/` (`<id>-<version>.bin` + `catalog.json`: id, name, version, description,
  requires, size, sha256, firmware URL, base64 512-byte icon; root has `format`,
  `protocol` = install protocol version, `release` tag). `--release` publishes a GitHub
  Release marked latest (needs a clean, pushed tree); the app reads
  `https://github.com/Mordokay/Dotty/releases/latest/download/catalog.json`. Bump the
  version in `DOTTY_CARTRIDGE()` when a cartridge changes. Optional per-cartridge
  `cartridges/<id>/cartridge.json` = description + requires.
- Measured from the Mac: 2M PHY, ~30 ms interval, 7-9.5 KB/s (856 KB music ≈ 90 s;
  occasionally much slower right after another transfer). Expect better from iOS.
- Power: wake lock `kWakeLockBle` while connected. Before light sleep the shell calls
  `ble::stop()` (NimBLE deinit; the controller can't sleep without a 32 kHz crystal) and
  `ble::start()` on unlock — so Dotty is only reachable while unlocked or awake.
- Test from the Mac: `.venv/bin/python tools/ble_dotty.py scan|info|listen|cmd <name> k=v|install <bin>`
  (`install --corrupt` checks the SHA-256 rejection). Cartridge icons: `cartridges/<id>/icon.png`.
  (venv setup in the script's docstring; bleak; Bluetooth permission already granted).

## Power management (`lib/dotty_core/src/power.*`)

- Wake locks (`power::setWakeLock`) = "do not interrupt": while any is held the CPU
  never sleeps. Audio holds one while playing; Wi-Fi/BLE/OTA should add their own.
- Locked + no wake lock + no USB host: touch hibernates, codec/amp power down, audio
  rail off, CPU light-sleeps until the next minute (or PWR). Unlock wakes peripherals.
- Never sleeps while a computer is connected over USB (`HWCDC::isPlugged()`), so
  flashing and logs keep working; sleep can only be tested on battery or a wall charger.
- GPIO17 (power latch) is `gpio_hold_en`'d for life; GPIO6/42/46 are held during sleep.
- Power off on USB power: the latch can't cut power, so it deep-sleeps with PWR (ext0)
  as the wake source; waking is a fresh boot.
- Battery voltage is logged every 10 minutes (`power: battery ... mV`) to measure drain.

## iOS app (`ios/`)

- SwiftUI app `ios/Dotty.xcodeproj` (target/scheme `Dotty`, bundle id
  `com.greenspherestudios.dotty`, personal team `DL6N525G5K`, iOS 26.6+, Swift 6). Look and feel = the firefly design
  system in `ios/Dotty/DesignSystem/`, documented in `ios/Design/System/project/README.md`
  (tokens in `tokens.json`; rebuild the JS preview bundle with `python3 ios/Design/System/src/build.py`).
- Build check without signing:
  `xcodebuild -project ios/Dotty.xcodeproj -scheme Dotty -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build`
- Signing uses the user's **personal** team (picked in Xcode); never the company team.
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
