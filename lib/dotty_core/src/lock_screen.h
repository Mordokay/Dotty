#pragma once

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <time.h>

struct LockScreenInfo {
  tm time;
  bool timeValid;
  uint8_t batteryPercent;
  bool externalPower;  // on a charger or computer
  bool charging;       // external power, not full yet
  bool batteryLow;
  String nowPlaying;  // empty when no music is playing
};

// Kindle-style lock screen: clock, date, battery and a padlock. On a charger the padlock
// makes way for a big battery filling up, with a bolt and the percentage. Meant to be
// redrawn once a minute.
void drawLockScreen(Adafruit_GFX &gfx, const LockScreenInfo &info);

// Shown just before Dotty switches itself off with an empty battery (e-paper keeps it).
void drawBatteryEmpty(Adafruit_GFX &gfx);
