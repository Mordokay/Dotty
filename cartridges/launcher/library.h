#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

// Cartridges kept on the SD card, so switching to one already there takes seconds
// instead of a BLE transfer. Layout per version:
//   /cartridges/<id>/firmware/<version>.bin   firmware image
//   /cartridges/<id>/firmware/<version>.json  {id, name, version, size, sha256}
//   /cartridges/<id>/firmware/<version>.icon  64x64 1-bit icon (optional)
// Every use re-checks the image against its SHA-256 before it is booted.
namespace library {

struct Entry {
  String id, name, version, sha256;  // sha256 as 64 hex characters
  size_t size = 0;
  bool hasIcon = false;
};

using Progress = std::function<void(size_t done, size_t total)>;

bool begin();  // mounts the card; false if there is none
bool available();

std::vector<Entry> list();
// version empty = the newest one on the card.
bool find(const String &id, const String &version, Entry &out);
bool readIcon(const Entry &entry, uint8_t *out);  // kIconBytes

// Paths for writing a new version (e.g. a download): write the image to tempPath(),
// then commit() renames it and writes the metadata + icon.
String tempPath(const String &id, const String &version);
bool commit(const Entry &entry, const uint8_t *icon, String &error);

// Copies the cartridge just installed in ota_0 onto the card (verifying its SHA-256).
bool saveInstalled(const Entry &entry, const uint8_t *icon, String &error);

// Flashes an entry from the card into ota_0 and selects it for the next boot.
bool install(const Entry &entry, Progress progress, String &error);

}  // namespace library
