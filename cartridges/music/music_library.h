#pragma once

#include <Arduino.h>

#include <vector>

// Songs and playlists in the Music cartridge's data folder on the SD card:
//   library/<song>.mp3       every song, stored once
//   playlists/<name>.m3u     a list of songs (lines "../library/<song>.mp3"), so the card
//                            also works in computer music players
namespace music {

struct Song {
  String name;  // file name in library/
  size_t size = 0;
  time_t added = 0;  // the file's date on the card (when it arrived)
};

void begin();
void rescan();

const std::vector<Song> &songs();  // sorted by name
bool hasSong(const String &name);
String songPath(const String &name);
bool deleteSong(const String &name);  // also drops it from every playlist

std::vector<String> playlists();  // sorted
std::vector<String> playlistSongs(const String &playlist);  // only songs still in the library
bool createPlaylist(const String &name);
bool deletePlaylist(const String &name);
// False if the new name is empty or taken. Playlist names are storage::safeName'd.
bool renamePlaylist(const String &name, const String &newName);
bool addToPlaylist(const String &playlist, const std::vector<String> &songNames);
bool removeFromPlaylist(const String &playlist, const String &song);
// Moves the song at `from` to `to` (positions in playlistSongs()).
bool moveInPlaylist(const String &playlist, int from, int to);
// Reorders a playlist by title (A-Z) or by date added (newest first).
bool sortPlaylist(const String &playlist, bool byDateAdded);

// "NAPA-Deslocado.mp3" → "NAPA - Deslocado"
String title(const String &songName);

}  // namespace music
