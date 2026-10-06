#pragma once

#include <Arduino.h>

#include "epd_display.h"
#include "lock_screen.h"
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
  // Periodic work while locked (e.g. fetch the weather). Every lockedWakeSeconds the
  // shell wakes the firmware (even from sleep) and calls onLockedWake on the main loop;
  // return true if the lock screen should be redrawn. 0 = no extra wake-ups (the lock
  // screen itself still updates its clock once a minute).
  uint32_t lockedWakeSeconds = 0;
  bool (*onLockedWake)() = nullptr;
  // Bluetooth while locked. false (default): locking turns Bluetooth off (the phone
  // disconnects) so Dotty can sleep between minute wake-ups; unlocking turns it back on.
  // true: a connected phone stays connected while locked (e.g. a cartridge receiving
  // iPhone notifications); Dotty then stays awake while connected. OFF always cuts it.
  bool bluetoothWhileLocked = false;
  // Optional content for the lock screen (e.g. a joke, the forecast): draw it from y = 0 on
  // the full-width canvas given (at most maxHeight tall: 4 lines of 9 pt text) and return the
  // height used, 0 for nothing. The clock and a small padlock then share one row, and the
  // row plus this content are centred together. 0 keeps the usual layout.
  int16_t (*drawLockWidget)(Adafruit_GFX &gfx, const tm &now, int16_t maxHeight) = nullptr;
  // Replaces the whole lock screen (e.g. a photo as a screensaver). Draw everything into
  // gfx (::drawLockScreen(gfx, info) gives the usual one) and return true when the picture
  // changed and needs a full refresh (partial refreshes leave ghosts of a photo), false for
  // a partial one. Called once a minute, and when the battery or power source changes.
  bool (*lockScreen)(Adafruit_GFX &gfx, const LockScreenInfo &info) = nullptr;
  // Power-off pictures; one is picked at random each time.
  const Picture *offPictures = nullptr;
  size_t offPictureCount = 0;
};

struct Input {
  Touch::Gesture gesture = Touch::Gesture::None;
  bool boot = false;  // BOOT clicked (pressed and released on its own)
  char key = 0;       // a key typed on the serial monitor (developer aid), 0 if none
};

// Sets the clock (RTC and system time) to this local time, e.g. from internet time.
void setLocalTime(time_t local);
// Seconds to add to UTC for local time, as the phone last said (core.time {utcOffset}); kept
// in NVS. False until a phone has sent it. Dotty's clock keeps local time, so anything dated in
// UTC (e.g. news) needs it.
bool utcOffset(int32_t &seconds);

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
