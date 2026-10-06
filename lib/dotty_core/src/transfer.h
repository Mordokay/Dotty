#pragma once

#include <Arduino.h>

#include <functional>

// Fast file uploads from the phone over the local Wi-Fi (Bluetooth would be ~20-50x slower).
//
//   1. BLE transfer.start → Dotty joins its best saved network, starts an HTTP server and
//      replies {url, token, ssid, rssi, bluetooth: "paused"}. The token travels only over the
//      encrypted BLE link. Then BLE pauses: Wi-Fi and BLE share the radio, and uploads run
//      ~2x faster without it.
//   2. The phone (same Wi-Fi) sends each file:
//        POST <url>/upload?dir=<folder>&name=<file>   header X-Dotty-Token: <token>
//      → {"ok": true, "name": <file name as stored>}. (PUT works too, but iOS silently
//      re-sends a PUT whose connection drops, hiding failures from the app.)
//      The file lands in the running cartridge's data folder (dir and name are cleaned so
//      nothing escapes it), written to <name>.part and renamed when complete.
//      An upload with no data for 30 s is abandoned (its .part file deleted; leftovers
//      from reboots are cleared when a session starts).
//      The other way: GET <url>/download?dir=<folder>&name=<file> (same header) streams a
//      file from the data folder to the phone (chunked).
//   3. POST <url>/done (same header), BLE transfer.stop, or 1 minute without uploads ends
//      the session: server and Wi-Fi off, BLE back on (the phone reconnects by itself).
// Events (while BLE is on): transfer.received {dir, name, size} after each file.
namespace transfer {

struct Status {
  bool active = false;
  String file;         // file being received ("" between files)
  size_t done = 0, total = 0;
  int filesReceived = 0;
  // A backup or restore (whole-card mode): the app says where it is with each file
  // (headers X-Dotty-Job: backup|restore, X-Dotty-Step: 20/50, X-Dotty-Bytes: before/total).
  // job "check" = Dotty fingerprinting its files for a restore (step = files done).
  String job;
  int step = 0, steps = 0;
  uint64_t bytesBefore = 0, bytesTotal = 0;
};

// How a session went, for the cartridge to show.
struct Summary {
  int received = 0;           // files that arrived complete
  int failed = 0;             // uploads that broke off
  bool appFinished = false;   // the app ended it (/done or transfer.stop), not the idle timeout
  bool ok() const { return failed == 0 && appFinished; }
};

// Registers transfer.start / transfer.stop. onFinished runs on the main loop when a
// session ends (e.g. to rescan the music library and show the result).
void registerCommands(std::function<void(const Summary &)> onFinished = nullptr);
// Call from the main loop: idle timeout and events.
void poll();
bool active();
// Stops the session from Dotty's side (e.g. a Cancel button): the app's next request gets
// "cancelled on Dotty", and the session ends a few seconds later.
void cancel();
bool cancelling();
Status status();

}  // namespace transfer
