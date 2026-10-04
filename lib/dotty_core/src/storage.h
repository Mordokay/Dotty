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

// "Road trip/../x" → "Road trip/x"-style cleaning: one path segment, no slashes or dots.
String safeName(const String &name);

}  // namespace storage
