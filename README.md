# Dotty

A battery-powered desk companion built on a 1.54" e-paper ESP32-S3 board. Dotty shows a
little face and status info on e-ink, and will grow into a Tamagotchi-style character
with moods driven by real sensors, controlled from a companion iOS app over Bluetooth LE.

## Hardware

- **Board:** Waveshare ESP32-S3-Touch-ePaper-1.54-EN (SKU 34212), V2
  - ESP32-S3-PICO-1-N8R8: 8 MB flash, 8 MB PSRAM, Wi-Fi + BLE 5
  - 200×200 black/white e-paper with FT6336 capacitive touch
  - SHTC3 temperature/humidity, PCF85063 RTC, ES8311 audio codec + mic + speaker, microSD slot
- **Battery:** Waveshare 3.7 V 400 mAh LiPo (SKU 32066), MX1.25 connector
- **microSD:** SanDisk Ultra 32 GB, FAT32 / MBR, label `DOTTY`, folders `/ui`, `/audio`, `/system`

Details, pin map and power notes: [docs/HARDWARE.md](docs/HARDWARE.md).

## Setup (macOS)

1. Install [VS Code](https://code.visualstudio.com/) and the **pioarduino IDE** extension.
   Don't install the official PlatformIO IDE extension alongside it — they conflict.
2. Let the extension install its core (first launch). The `pio` CLI then lives in
   `~/.platformio/penv/bin/pio`; symlink it into `~/.local/bin` to use it from a terminal.
3. Plug the board in with a data-capable USB-C cable. It shows up as `/dev/cu.usbmodem*`
   (native USB, no driver needed).

## Build, flash, monitor

From VS Code: the ✓ (build), → (upload) and 🔌 (serial monitor) buttons in the status bar.

From a terminal:

```bash
pio run -e music                 # build the music cartridge
pio run -e launcher -t upload    # flash the launcher (permanent, factory slot)
pio run -e music -t upload       # flash the music cartridge (cartridge slot)
pio device monitor       # serial output (Ctrl+C to quit)
```

The first build downloads the ESP32 toolchain (a few minutes). Only one program can hold
the serial port at a time — close the monitor before uploading from a terminal.

**Logs:** the firmware keeps the last 16 KB of log in memory. In the serial monitor,
type `d` to replay everything since boot, e.g. after plugging in later.

## Using the device

- **PWR short press:** lock / unlock, like a Kindle. The lock screen shows the clock,
  date, battery and a padlock, and redraws only once a minute. Music keeps
  playing while locked. Touch and BOOT are ignored while locked.
- **Auto-lock:** after 2 minutes without interaction.
- **Power saving:** while locked and nothing needs the CPU (e.g. no music playing), Dotty
  sleeps between the once-a-minute clock updates. It stays awake while a computer is
  connected over USB, so flashing and logs keep working.
- **PWR hold 2 s:** power off. Shows a random picture (portrait or sleeping red panda); on battery the board switches off
  (~10 µA, the RTC keeps time), on USB it deep-sleeps. Press PWR to start again.
- **Player:** tap the middle to play/pause, the − / + circles for volume.
  Long-press anywhere for a full refresh (clears ghosting).
- **BOOT button:** switches between the player and the e-paper refresh test.
- **BOOT + PWR held 1 s:** leave the cartridge for the launcher. The launcher shows the
  installed cartridge; press BOOT there to start it again.
- **Power on:** USB power boots straight away; on battery hold **PWR** until the screen
  redraws.

## Project layout

```
platformio.ini        Build config — replaces the Arduino IDE "Tools" menu
partitions.csv        8 MB flash layout with two OTA app slots
cartridges/launcher/  Permanent launcher (factory partition): starts/installs cartridges
cartridges/<name>/    One firmware ("cartridge") per product, e.g. cartridges/music/
lib/dotty_core/src/   Shared code: display driver, power, logger, touch, RTC, UI, audio
lib/dotty_core/src/board_pins.h   Every GPIO on the board
cartridges/<name>/images/        Generated 1-bit bitmaps (see tools/img2epd.py)
tools/img2epd.py      Converts a picture into a dithered e-paper bitmap header
tools/ble_dotty.py    Talks to Dotty over Bluetooth from the Mac (info, commands)
ios/                  Dotty iOS app (SwiftUI) and its design system docs
docs/HARDWARE.md      Hardware reference
```

## Roadmap

- [x] Toolchain, SD card, "Hello, Dotty" on e-paper with full + partial refresh
- [ ] Dotty's face and expressions
- [ ] Sensors: SHTC3 room temperature/humidity, RTC clock, battery level
- [ ] Touch input
- [ ] BLE command service (testable with nRF Connect)
- [ ] iOS companion app: settings, Wi-Fi setup, firmware updates over BLE (OTA)

## Restoring the factory demo

```bash
esptool --port /dev/cu.usbmodem1101 write-flash 0x0 V2-FactoryProgram.bin
```

`V2-FactoryProgram.bin` is in `03_Firmware/` of
[waveshareteam/ESP32-S3-ePaper-1.54](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54).
