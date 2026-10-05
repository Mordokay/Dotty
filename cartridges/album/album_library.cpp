#include "album_library.h"

#include <SD_MMC.h>

#include <algorithm>

#include "log.h"
#include "storage.h"

namespace album {
namespace {

std::vector<Photo> library;

String photoDir() {
  return storage::myDataDir() + "/photos";
}

String albumDir() {
  return storage::myDataDir() + "/albums";
}

String albumPath(const String &name) {
  return albumDir() + "/" + storage::safeName(name) + ".txt";
}

String photoPath(const String &name) {
  return photoDir() + "/" + name;
}

bool isPhoto(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return !name.startsWith(".") && lower.endsWith(".pbm");
}

std::vector<String> readAlbum(const String &name) {
  std::vector<String> entries;
  File f = SD_MMC.open(albumPath(name));
  while (f && f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() && !line.startsWith("#")) entries.push_back(line);
  }
  return entries;
}

bool writeAlbum(const String &name, const std::vector<String> &entries) {
  File f = SD_MMC.open(albumPath(name), FILE_WRITE);
  if (!f) return false;
  for (const String &photo : entries) f.print(photo + "\n");
  return true;
}

// PBM header: "P4", width, height, separated by whitespace (and # comments), then one
// whitespace byte before the bitmap.
bool readHeaderNumber(File &f, int &out) {
  int c = f.read();
  while (c == '#' || isspace(c)) {
    if (c == '#') {
      while (c >= 0 && c != '\n') c = f.read();
    }
    c = f.read();
  }
  if (!isdigit(c)) return false;
  out = 0;
  while (isdigit(c)) {
    out = out * 10 + (c - '0');
    c = f.read();
  }
  return isspace(c);  // the single byte before the bitmap (or between the numbers)
}

}  // namespace

void begin() {
  storage::makeDirs(photoDir());
  storage::makeDirs(albumDir());
  rescan();
}

void rescan() {
  library.clear();
  File dir = SD_MMC.open(photoDir());
  for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
    const String name = f.name();
    if (!f.isDirectory() && isPhoto(name)) library.push_back({name, f.getLastWrite()});
  }
  std::sort(library.begin(), library.end(), [](const Photo &a, const Photo &b) { return a.name < b.name; });
  LOGI("album", "%u photos, %u albums", static_cast<unsigned>(library.size()), static_cast<unsigned>(albums().size()));
}

const std::vector<Photo> &photos() {
  return library;
}

bool hasPhoto(const String &name) {
  return std::any_of(library.begin(), library.end(), [&](const Photo &p) { return p.name == name; });
}

bool loadPhoto(const String &name, uint8_t *bitmap) {
  File f = SD_MMC.open(photoPath(name));
  if (!f || f.read() != 'P' || f.read() != '4') return false;
  int w = 0, h = 0;
  if (!readHeaderNumber(f, w) || !readHeaderNumber(f, h) || w != kSize || h != kSize) {
    LOGW("album", "%s: not a %dx%d P4 picture", name.c_str(), kSize, kSize);
    return false;
  }
  return f.read(bitmap, kBitmapBytes) == kBitmapBytes;
}

bool deletePhoto(const String &name) {
  if (!hasPhoto(name)) return false;
  SD_MMC.remove(photoPath(name));
  for (const String &a : albums()) removeFromAlbum(a, name);
  rescan();
  return true;
}

std::vector<String> albums() {
  std::vector<String> names;
  File dir = SD_MMC.open(albumDir());
  for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
    String name = f.name();
    if (f.isDirectory() || !name.endsWith(".txt") || name.startsWith(".")) continue;
    name.remove(name.length() - 4);
    names.push_back(name);
  }
  std::sort(names.begin(), names.end(), [](const String &a, const String &b) {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
  });
  return names;
}

std::vector<String> albumPhotos(const String &name) {
  std::vector<String> present;
  if (name.isEmpty()) {
    for (const Photo &p : library) present.push_back(p.name);
    return present;
  }
  for (const String &photo : readAlbum(name)) {
    if (hasPhoto(photo)) present.push_back(photo);
  }
  return present;
}

bool albumExists(const String &name) {
  return name.length() && SD_MMC.exists(albumPath(name));
}

bool createAlbum(const String &name) {
  if (storage::safeName(name).isEmpty()) return false;
  if (SD_MMC.exists(albumPath(name))) return true;
  return writeAlbum(name, {});
}

bool deleteAlbum(const String &name) {
  return SD_MMC.remove(albumPath(name));
}

bool renameAlbum(const String &name, const String &newName) {
  if (storage::safeName(newName).isEmpty() || !SD_MMC.exists(albumPath(name))) return false;
  if (storage::safeName(newName) == storage::safeName(name)) return true;
  if (SD_MMC.exists(albumPath(newName))) return false;
  return SD_MMC.rename(albumPath(name), albumPath(newName));
}

bool addToAlbum(const String &name, const std::vector<String> &photoNames) {
  if (!SD_MMC.exists(albumPath(name))) return false;
  std::vector<String> entries = readAlbum(name);
  for (const String &photo : photoNames) {
    if (hasPhoto(photo) && std::find(entries.begin(), entries.end(), photo) == entries.end()) {
      entries.push_back(photo);
    }
  }
  return writeAlbum(name, entries);
}

bool removeFromAlbum(const String &name, const String &photo) {
  std::vector<String> entries = readAlbum(name);
  const auto end = std::remove(entries.begin(), entries.end(), photo);
  if (end == entries.end()) return true;
  entries.erase(end, entries.end());
  return writeAlbum(name, entries);
}

bool moveInAlbum(const String &name, int from, int to) {
  if (name.isEmpty()) return false;  // "every photo" is in date order
  std::vector<String> list = albumPhotos(name);
  const int count = list.size();
  if (from < 0 || from >= count || to < 0 || to >= count) return false;
  const String moved = list[from];
  list.erase(list.begin() + from);
  list.insert(list.begin() + to, moved);
  return writeAlbum(name, list);
}

String dateTaken(const String &photoName) {
  static const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int y, m, d;
  if (sscanf(photoName.c_str(), "%4d%2d%2d-", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return "";
  return String(d) + " " + kMonths[m - 1] + " " + String(y);
}

}  // namespace album
