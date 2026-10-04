#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <functional>

// The "Dotty Core" BLE service, identical in the launcher and every cartridge.
//
//   Info    (read)            JSON: role, id, name, version, battery, commands, …
//   Command (write)           JSON: {"cmd": "music.volume", "value": 60}
//   Event   (notify)          JSON: replies {"cmd": …, "ok": true, …} and status updates
//   Data    (write)           bulk bytes (cartridge installs), acknowledged writes
//
// Advertising carries the name "Dotty-XXXX" plus manufacturer data 0xFFFF + the
// 6-byte chip serial (factory MAC), so an app can tell Dottys apart while scanning
// (iOS hides real MAC addresses from apps).
//
// Commands arrive on the BLE task and are queued; poll() runs them on the caller's
// task (the main loop), so handlers can safely touch the display, audio, etc.
namespace ble {

// args: the whole command object. reply: pre-filled with "cmd"; set "ok" and any
// result fields. Throwing is not needed: leaving "ok" unset means ok = true.
using Handler = std::function<void(JsonObjectConst args, JsonObject reply)>;

// Adds fields to the Info JSON (e.g. the launcher's installed cartridge).
using InfoExtender = std::function<void(JsonObject info)>;

// Receives Data characteristic writes. Runs on the BLE task: copy and return fast.
using DataHandler = std::function<void(const uint8_t *data, size_t len)>;

// Device name "Dotty-XXXX" (last MAC bytes). Starts advertising.
void begin();
void on(const char *cmd, Handler handler);
void extendInfo(InfoExtender extender);
void onData(DataHandler handler);

// Chip serial, "70:04:1D:D7:B1:00" (the factory MAC; stable, unique per board).
const String &serial();

// Runs queued commands and refreshes the Info value. Call from the main loop.
void poll();

// Sends a JSON event to the connected app (no-op when nothing is connected).
void notify(JsonDocument &event);

bool connected();

// Bluetooth off/on around light sleep (the controller can't sleep on this board).
void stop();
void start();
bool running();

}  // namespace ble
