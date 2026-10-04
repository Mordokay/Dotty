# Dotty hardware reference

## Board
**Waveshare ESP32-S3-Touch-ePaper-1.54-EN** (SKU 34212), V2 silkscreen.
Battery: Waveshare 3.7 V 400 mAh LiPo (SKU 32066), MX1.25 connector.

Verified on the device with `esptool flash-id` and the boot log:

| | |
|---|---|
| Module | ESP32-S3-PICO-1-N8R8 — ESP32-S3 rev 2, 2 cores @ 240 MHz |
| Flash | 8 MB (GD, quad) |
| PSRAM | 8 MB OPI (AP 3.3 V) |
| Radio | Wi-Fi 2.4 GHz b/g/n, **BLE 5 only** (no Bluetooth Classic → no A2DP audio streaming) |
| USB | Native USB-Serial/JTAG, shows up as `/dev/cu.usbmodem*` (VID 303A), no driver needed |

## Peripherals
| Part | What | Notes |
|---|---|---|
| e-Paper | 1.54" 200×200 B/W, SSD1681-class | Full refresh ≈ 1.9 s (flashes), partial ≈ 0.6 s. Keeps image with power off. |
| FT6336 | Capacitive touch (I2C) | INT GPIO21, RST GPIO7 |
| SHTC3 | Temperature + humidity (I2C 0x70) | Real indoor readings |
| PCF85063 | RTC (I2C 0x51) | Timekeeping, wake from deep sleep |
| ES8311 | Audio codec | Onboard mic + onboard speaker, MX1.25 speaker header |
| ETA6098 | LiPo charger | Battery voltage on GPIO4 via 1:2 divider |
| TF slot | microSD, SDMMC 1-bit | Card formatted FAT32/MBR, label `DOTTY`, folders `/ui /audio /system` |
| LED | GPIO3 | |
| Buttons | BOOT (GPIO0), PWR (GPIO18) | Both active-low |

There is **no IMU** (no shake detection) and no ES7210.

Full pin map: [`lib/dotty_core/src/board_pins.h`](../lib/dotty_core/src/board_pins.h).

## Power
- On battery, the board stays on only while firmware holds **GPIO17 high** (soft power latch).
  Long-press PWR to power on; current firmware releases the latch after a 2 s PWR hold.
- GPIO6 LOW powers the e-paper (EPD3V3 via a P-MOSFET), GPIO42 LOW powers the audio rail
  (ES8311, mic, NS4150B amp). With the audio rail off the unpowered codec clamps the I2C bus.
- Always on (3V3): touch, RTC, SHTC3, SD card, I2C pull-ups (4.7 kΩ), battery divider
  (2 × 200 kΩ, ~10 µA).
- Charger: ETA6098, 0.2 A charge current (R35 = 820 kΩ). Its STAT pin only drives the orange LED.

## Flash layout (`partitions.csv`)
Two 3 MB app slots (`app0`/`app1`) for BLE OTA updates from the iOS app, 1.9 MB `spiffs`
data partition (LittleFS), 64 KB coredump.

## Restoring the factory demo
```
esptool --port /dev/cu.usbmodem1101 write-flash 0x0 V2-FactoryProgram.bin
```
from `03_Firmware/` in https://github.com/waveshareteam/ESP32-S3-ePaper-1.54.

## Sources
- https://docs.waveshare.com/ESP32-S3-ePaper-1.54
- https://github.com/waveshareteam/ESP32-S3-ePaper-1.54
