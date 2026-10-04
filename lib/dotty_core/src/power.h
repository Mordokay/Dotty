#pragma once

#include <Arduino.h>

// Board power: soft power latch, power rails, wake locks and sleep.
namespace power {

// "Do not interrupt": while any wake lock is held the CPU never sleeps.
// Add a bit per feature that must keep running (audio now; Wi-Fi sync, BLE
// transfers and OTA later).
enum WakeLock : uint32_t {
  kWakeLockAudio = 1 << 0,
  kWakeLockBle = 1 << 1,      // an app is connected
  kWakeLockNetwork = 1 << 2,  // a Wi-Fi transfer is running
};

// Latches system power on (call first thing in setup).
void begin();

void setWakeLock(uint32_t lock, bool held);
uint32_t wakeLocks();

// True while a computer is talking to the USB port: stay awake for debugging/flashing.
bool usbHostConnected();

// Audio rail (GPIO42): codec, mic and amplifier. While it is off the unpowered
// codec drags the shared I2C bus down, so touch and the RTC are unreachable too.
void setAudioRail(bool on);

// Light-sleeps for up to ms, or until PWR is pressed. All tasks pause; RAM,
// display contents and GPIO levels are kept. Returns true if woken by PWR.
bool lightSleep(uint32_t ms);

// Releases the power latch. On battery the board switches off right here; on
// USB power it keeps running, so it deep-sleeps until PWR is pressed instead
// (waking is a fresh boot). Never returns.
[[noreturn]] void shutdown();

}  // namespace power
