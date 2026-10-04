#pragma once

#include <Arduino.h>

#include <functional>

// Fast file uploads from the phone over the local Wi-Fi (Bluetooth would be ~20-50x slower).
//
//   1. BLE transfer.start → Dotty joins its best saved network, starts an HTTP server and
//      replies {url, token, ssid, bluetooth: "paused"}. The token travels only over the
//      encrypted BLE link. Then BLE pauses: Wi-Fi and BLE share the radio, and uploads run
//      ~2x faster without it.
//   2. The phone (same Wi-Fi) sends each file:
//        PUT <url>/upload?dir=<folder>&name=<file>   header X-Dotty-Token: <token>
//      The file lands in the running cartridge's data folder (dir and name are cleaned so
//      nothing escapes it), written to <name>.part and renamed when complete.
//   3. POST <url>/done (same header), BLE transfer.stop, or 2 minutes without uploads ends
//      the session: server and Wi-Fi off, BLE back on (the phone reconnects by itself).
// Events (while BLE is on): transfer.received {dir, name, size} after each file.
namespace transfer {

struct Status {
  bool active = false;
  String file;         // file being received ("" between files)
  size_t done = 0, total = 0;
  int filesReceived = 0;
};

// Registers transfer.start / transfer.stop. onFinished runs on the main loop when a
// session ends (e.g. to rescan the music library).
void registerCommands(std::function<void()> onFinished = nullptr);
// Call from the main loop: idle timeout and events.
void poll();
bool active();
Status status();

}  // namespace transfer
