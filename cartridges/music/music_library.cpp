#include "music_library.h"

#include <SD_MMC.h>

#include <algorithm>

#include "log.h"
#include "storage.h"

namespace music {
namespace {

std::vector<Song> library;

String libraryDir() {
  return storage::myDataDir() + "/library";
}

String playlistDir() {
  return storage::myDataDir() + "/playlists";
}

String playlistPath(const String &name) {
  return playlistDir() + "/" + storage::safeName(name) + ".m3u";
}

bool isSong(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return !name.startsWith(".") && lower.endsWith(".mp3");
}

// Raw entries of a playlist file (song names), in order.
std::vector<String> readPlaylist(const String &name) {
  std::vector<String> entries;
  File f = SD_MMC.open(playlistPath(name));
  while (f && f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) continue;
    entries.push_back(line.substring(line.lastIndexOf('/') + 1));
  }
  return entries;
}

bool writePlaylist(const String &name, const std::vector<String> &entries) {
  File f = SD_MMC.open(playlistPath(name), FILE_WRITE);
  if (!f) return false;
  f.print("#EXTM3U\n");
  for (const String &song : entries) f.print("../library/" + song + "\n");
  return true;
}

}  // namespace

void begin() {
  storage::makeDirs(libraryDir());
  storage::makeDirs(playlistDir());
  rescan();
}

void rescan() {
  library.clear();
  File dir = SD_MMC.open(libraryDir());
  for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
    const String name = f.name();
    if (!f.isDirectory() && isSong(name)) library.push_back({name, f.size(), f.getLastWrite()});
  }
  std::sort(library.begin(), library.end(), [](const Song &a, const Song &b) { return a.name < b.name; });
  LOGI("music", "%u songs, %u playlists", static_cast<unsigned>(library.size()),
       static_cast<unsigned>(playlists().size()));
}

const std::vector<Song> &songs() {
  return library;
}

bool hasSong(const String &name) {
  return std::any_of(library.begin(), library.end(), [&](const Song &s) { return s.name == name; });
}

String songPath(const String &name) {
  return libraryDir() + "/" + name;
}

bool deleteSong(const String &name) {
  if (!hasSong(name)) return false;
  SD_MMC.remove(songPath(name));
  for (const String &playlist : playlists()) removeFromPlaylist(playlist, name);
  rescan();
  return true;
}

std::vector<String> playlists() {
  std::vector<String> names;
  File dir = SD_MMC.open(playlistDir());
  for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
    String name = f.name();
    if (f.isDirectory() || !name.endsWith(".m3u") || name.startsWith(".")) continue;
    name.remove(name.length() - 4);
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<String> playlistSongs(const String &playlist) {
  std::vector<String> present;
  for (const String &song : readPlaylist(playlist)) {
    if (hasSong(song)) present.push_back(song);
  }
  return present;
}

bool createPlaylist(const String &name) {
  if (storage::safeName(name).isEmpty()) return false;
  if (SD_MMC.exists(playlistPath(name))) return true;
  return writePlaylist(name, {});
}

bool deletePlaylist(const String &name) {
  return SD_MMC.remove(playlistPath(name));
}

bool renamePlaylist(const String &name, const String &newName) {
  if (storage::safeName(newName).isEmpty() || !SD_MMC.exists(playlistPath(name))) return false;
  if (storage::safeName(newName) == storage::safeName(name)) return true;
  if (SD_MMC.exists(playlistPath(newName))) return false;
  return SD_MMC.rename(playlistPath(name), playlistPath(newName));
}

bool addToPlaylist(const String &playlist, const std::vector<String> &songNames) {
  if (!SD_MMC.exists(playlistPath(playlist))) return false;
  std::vector<String> entries = readPlaylist(playlist);
  for (const String &song : songNames) {
    if (hasSong(song) && std::find(entries.begin(), entries.end(), song) == entries.end()) {
      entries.push_back(song);
    }
  }
  return writePlaylist(playlist, entries);
}

bool removeFromPlaylist(const String &playlist, const String &song) {
  std::vector<String> entries = readPlaylist(playlist);
  const auto end = std::remove(entries.begin(), entries.end(), song);
  if (end == entries.end()) return true;
  entries.erase(end, entries.end());
  return writePlaylist(playlist, entries);
}

bool moveInPlaylist(const String &playlist, int from, int to) {
  // Rewritten from the songs still in the library, so positions match playlistSongs().
  std::vector<String> songs = playlistSongs(playlist);
  const int count = songs.size();
  if (from < 0 || from >= count || to < 0 || to >= count) return false;
  const String moved = songs[from];
  songs.erase(songs.begin() + from);
  songs.insert(songs.begin() + to, moved);
  return writePlaylist(playlist, songs);
}

String title(const String &songName) {
  String name = songName;
  const int dot = name.lastIndexOf('.');
  if (dot > 0) name.remove(dot);
  name.replace("_", " ");
  name.replace(" - ", "-");
  name.replace("-", " - ");
  return name;
}

}  // namespace music
