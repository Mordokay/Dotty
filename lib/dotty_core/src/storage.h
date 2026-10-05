#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

// The SD card, shared by the launcher and every cartridge. Each cartridge owns a folder:
//   /cartridges/<id>/firmware/  versions kept by the launcher (<version>.bin/.json/.icon)
//   /cartridges/<id>/data/      the cartridge's own files (music: library/ + playlists/)
namespace storage {

constexpr const char *kRoot = "/cartridges";

// Mounts the card (1-bit SDMMC) and migrates the older layouts once. False if no card.
bool begin();
bool available();

String cartridgeDir(const String &id);
String firmwareDir(const String &id);
String dataDir(const String &id);
// The running firmware's own data folder (created on demand).
String myDataDir();

void makeDirs(const String &path);  // like mkdir -p
bool removeTree(const String &path);

// What a cartridge has on the card.
struct CartridgeFiles {
  String id;
  std::vector<String> versions;  // firmware copies kept by the launcher
  uint64_t dataBytes = 0;        // its data folder (e.g. songs and playlists)
};
std::vector<CartridgeFiles> cartridges();

// Deletes a cartridge's firmware copies; with withData also its data folder and its NVS
// settings (namespace = its id), otherwise those stay for a later reinstall. The running
// firmware can't remove itself.
bool removeCartridge(const String &id, bool withData, String &error);
// Runs before a removal (the launcher empties the slot if that cartridge is installed).
void onRemove(std::function<void(const String &id)> hook);

// BLE: storage.list → {cartridges: [{id, versions, data}], free}, storage.remove {id, data}.
void registerCommands();

// One path segment: no slashes, colons or control characters, no leading dots; names
// longer than 120 bytes are shortened, keeping the extension and whole UTF-8 characters.
String safeName(const String &name);

}  // namespace storage
