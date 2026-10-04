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
- Buttons are active-low with pull-ups. The PWR button is still held down right after a
  battery power-on, so ignore it until it has been released once.
- Battery: `analogReadMilliVolts(4) * 2`.
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
~/.platformio/penv/bin/pio run                                         # build
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/cu.usbmodem1101
~/.platformio/penv/bin/esptool --port /dev/cu.usbmodem1101 flash-id    # chip info
```

- Serial monitor is interactive; to capture output non-interactively, open the port with
  pyserial from `~/.platformio/penv/bin/python`, pulse RTS to reset, and read lines.
- "Port is busy" on upload means the user's VS Code serial monitor is open — ask them to
  close it rather than killing their process.
- Destructive disk operations (e.g. `diskutil eraseDisk`) are blocked for Claude: give the
  user the exact command to run with `!`.

## Architecture decisions

- **Partition table is OTA-ready from day one** (`partitions.csv`: two 3 MB app slots,
  1.9 MB `spiffs`/LittleFS, coredump). The long-term goal is firmware updates over BLE
  from an iOS app; don't switch to a no-OTA layout.
- **Display driver** (`src/epd_display.*`) subclasses `GFXcanvas1`: its buffer layout
  (MSB-first, 25 bytes/row, 1 = white) matches the controller RAM, so it's sent as-is.
  Init sequence and LUTs come from Waveshare's example driver. Draw with Adafruit GFX,
  then call `refreshFull()` or `refreshPartial()`. Do a full refresh every ~30 partials
  to clear ghosting.
- Pins live only in `include/board_pins.h`; source them from Waveshare's
  `02_Example/Arduino/*/user_config.h` when adding peripherals.
- LVGL (8.3.11 / 9.3.0) is supported by Waveshare but not used yet; Adafruit GFX is enough
  for now on a 1-bit 200×200 screen.

## Long-term goal

An iOS app as Dotty's control panel over a custom BLE GATT command stack: Wi-Fi
provisioning, settings/storage management, firmware updates (BLE OTA), and more.
Design firmware features to be configurable over BLE later.

## Reference

- Waveshare docs: https://docs.waveshare.com/ESP32-S3-ePaper-1.54
- Waveshare code: https://github.com/waveshareteam/ESP32-S3-ePaper-1.54 (clone to a
  scratch dir when needed — examples, schematic, factory firmware)
