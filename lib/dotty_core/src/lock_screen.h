#pragma once

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <time.h>

struct LockScreenInfo {
  tm time;
  bool timeValid;
  uint8_t batteryPercent;
  String nowPlaying;  // empty when no music is playing
};

// Kindle-style lock screen: clock, date, battery and a padlock.
// Meant to be redrawn once a minute.
void drawLockScreen(Adafruit_GFX &gfx, const LockScreenInfo &info);
