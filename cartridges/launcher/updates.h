#pragma once

#include <Arduino.h>

// Cartridge updates over Wi-Fi, without the phone: the launcher reads the GitHub catalog once
// a day (and from its Updates button), downloads a newer version of every cartridge that is on
// the card (and of the launcher itself) onto the card, and keeps the newest two versions of
// each. Nothing is flashed in the background: a cartridge's update installs from the card when
// it's started (BOOT, the picker or the app), the launcher's through Rescue from the Updates
// screen. Runs in its own task; anything else that needs Wi-Fi stops it first (stop()).
namespace updates {

struct Status {
  bool running = false;
  String step;               // "Joining Wi-Fi", "Checking", "Downloading News 0.2.7"
  int done = 0, total = 0;   // downloads in this check
  size_t bytes = 0, size = 0;  // the current download
  String error;              // the last check's problem ("" if it worked)
  uint32_t checkedAt = 0;    // local time of the last check that worked (0 = never)
  int downloaded = 0;        // versions the last check put on the card
};

void begin();
bool due();      // a day since the last check (an hour after a failed one), with Wi-Fi saved
void start();    // no-op while one runs
// Stops a check (between download chunks) and waits for it; keepWifi = someone else is about
// to use the connection.
void stop(bool keepWifi);
Status status();
bool takeFinished();  // true once after a check ended
// A launcher on the card newer than the running one ("" if none).
String newerLauncher();

}  // namespace updates
