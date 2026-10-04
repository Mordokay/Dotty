#pragma once

#include <Arduino.h>

// Writes a cartridge image received over BLE into the ota_0 partition.
//
// Protocol (driven by the app, see tools/ble_dotty.py):
//   1. Command install.begin {id, name, version, size, sha256, icon}
//   2. Data writes (unacknowledged, for speed), each = 4-byte little-endian offset +
//      payload. The payload stream is the 1-bit icon (kIconBytes, if "icon" was true)
//      followed by the image.
//   3. Unacknowledged writes can be dropped, so offsets are checked: on a gap Dotty
//      sends event install.resend {from} and ignores data until the app rewinds there.
//   4. Event install.progress {received, written, size} every kProgressStep bytes; the
//      app keeps at most kWindow bytes in flight beyond "received" (flow control).
//   5. Command install.end → either {"ok": false, "missingFrom": n} (lost data at the
//      very end: rewind and send again) or SHA-256 + image check → boot partition set.
// Data arrives on the BLE task and goes into a buffer; a writer task erases and writes
// the flash sequentially, so BLE never waits for flash.
namespace installer {

constexpr int16_t kIconSize = 64;
constexpr size_t kIconBytes = kIconSize * kIconSize / 8;
constexpr size_t kWindow = 48 * 1024;
constexpr size_t kProgressStep = 8 * 1024;
constexpr size_t kOffsetBytes = 4;

struct Meta {
  String id, name, version;
  size_t size = 0;
  uint8_t sha256[32] = {};
  bool hasIcon = false;
};

enum class Result { Done, Missing, Failed };

void begin();  // once at boot: buffer + writer task

// Starts an install; false + error if it can't (too big, flash error, …).
bool start(const Meta &meta, String &error);
// Feeds one Data write: offset + payload (BLE task).
void feed(const uint8_t *data, size_t len);
// Missing: payload bytes from missingFrom on never arrived; the install stays active.
// Done: verified, ota_0 selected for the next boot. Failed: see error, install ended.
Result finish(String &error, size_t &missingFrom);
void abort();

// A gap was seen: the app should resend from `from`. Each request is reported once.
bool takeResendRequest(size_t &from);

bool active();
const Meta &meta();
size_t payloadSize();  // icon + image
size_t received();     // payload bytes accepted in order (icon included)
size_t written();      // image bytes written to flash (icon excluded)
bool iconReady();
const uint8_t *icon();  // kIconSize x kIconSize, rows MSB-first, set bit = black

}  // namespace installer
