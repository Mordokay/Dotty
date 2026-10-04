#pragma once

#include <Arduino.h>

#include "epd_display.h"
#include "rtc_clock.h"
#include "touch.h"

// The behaviour every Dotty firmware shares: the Dotty Core BLE service, PWR (short
// press lock/unlock, 2 s power off), BOOT + PWR held 1 s (back to the launcher),
// auto-lock, the lock screen with light sleep between minute updates, and the
// power-off picture. A firmware supplies
// its own screen through Config and runs its logic only while update() returns true.
namespace shell {

struct Picture {
  const uint8_t *bitmap;
  int16_t width, height;
};

struct Config {
  // Draws the firmware's own (unlocked) screen into shell::epd. Required.
  void (*drawApp)() = nullptr;
  // Power the firmware's own peripherals down before sleeping / back up after unlock.
  void (*sleepApp)() = nullptr;
  void (*wakeApp)() = nullptr;
  // Bottom line of the lock screen, e.g. the song playing; empty = "press PWR to unlock".
  String (*nowPlaying)() = nullptr;
  // Last chance to stop things before the power-off picture is drawn.
  void (*beforePowerOff)() = nullptr;
  // Power-off pictures; one is picked at random each time.
  const Picture *offPictures = nullptr;
  size_t offPictureCount = 0;
};

struct Input {
  Touch::Gesture gesture = Touch::Gesture::None;
  bool boot = false;  // BOOT clicked (pressed and released on its own)
};

// Shared hardware, ready after begin().
extern EpdDisplay epd;
extern Touch touch;
extern RtcClock rtc;

// Power latch, logger, display, I2C, touch and RTC. The firmware then sets up its own
// parts and calls showApp().
void begin(const Config &config);

// First thing in loop(). Returns false while locked (the firmware should skip this
// pass); when unlocked, fills `input` with this pass's touch gesture and BOOT click.
bool update(Input &input);

// Draws the firmware's screen and starts a full refresh (clean transition).
void showApp();

// Starts a refresh without waiting for the panel. With autoFull, every
// kPartialRefreshesPerFull-th partial becomes a full refresh to clear ghosting.
void refresh(bool full, bool autoFull = true);
int partialsSinceFull();

bool locked();
void lock();

// Unlocks if locked and restarts the auto-lock timer. Call while something the user
// is watching is in progress (e.g. a cartridge install).
void wake();

}  // namespace shell
