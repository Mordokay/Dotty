#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <functional>

// The "Dotty Core" BLE service, identical in the launcher and every cartridge.
//
//   Info    (read)            JSON: role, id, name, version, battery, commands, …
//   Command (write)           JSON: {"cmd": "music.volume", "value": 60}
//   Event   (notify)          JSON: replies {"cmd": …, "ok": true, …} and status updates
//   Data    (write, no resp)  bulk bytes (cartridge installs, later)
//
// Commands arrive on the BLE task and are queued; poll() runs them on the caller's
// task (the main loop), so handlers can safely touch the display, audio, etc.
namespace ble {

// args: the whole command object. reply: pre-filled with "cmd"; set "ok" and any
// result fields. Throwing is not needed: leaving "ok" unset means ok = true.
using Handler = std::function<void(JsonObjectConst args, JsonObject reply)>;

// Adds fields to the Info JSON (e.g. the launcher's installed cartridge).
using InfoExtender = std::function<void(JsonObject info)>;

// Device name "Dotty-XXXX" (last MAC bytes). Starts advertising.
void begin();
void on(const char *cmd, Handler handler);
void extendInfo(InfoExtender extender);

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
