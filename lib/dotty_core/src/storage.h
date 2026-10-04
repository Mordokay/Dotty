#pragma once

#include <Arduino.h>

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

// One path segment: no slashes, colons or control characters, no leading dots; names
// longer than 120 bytes are shortened, keeping the extension and whole UTF-8 characters.
String safeName(const String &name);

}  // namespace storage
