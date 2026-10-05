#pragma once

#include <Arduino.h>

#include <vector>

// Photos and albums in the Album cartridge's data folder on the SD card:
//   photos/<name>.pbm   every photo, stored once: 200 x 200, 1 bit, binary PBM ("P4"), so
//                       a computer can open them too. The app names them after the date
//                       taken ("20240714-193205.pbm"), so name order = date order.
//   albums/<name>.txt   an album: one photo name per line, in the album's order. Deleting
//                       an album keeps its photos; deleting a photo drops it from every album.
namespace album {

constexpr int kSize = 200;
constexpr size_t kBitmapBytes = kSize * kSize / 8;  // 5000: rows of 25 bytes, 1 = black

struct Photo {
  String name;
  time_t added = 0;  // the file's date on the card (when it arrived)
};

void begin();
void rescan();

const std::vector<Photo> &photos();  // by name (date taken)
bool hasPhoto(const String &name);
// The 5000 bitmap bytes (as drawBitmap wants them: MSB first, 1 = black).
bool loadPhoto(const String &name, uint8_t *bitmap);
bool deletePhoto(const String &name);  // also drops it from every album

std::vector<String> albums();                      // sorted
std::vector<String> albumPhotos(const String &album);  // "" = every photo; only photos on the card
bool albumExists(const String &album);
bool createAlbum(const String &name);
bool deleteAlbum(const String &name);
bool renameAlbum(const String &name, const String &newName);  // false if empty or taken
bool addToAlbum(const String &album, const std::vector<String> &photoNames);
bool removeFromAlbum(const String &album, const String &photo);
bool moveInAlbum(const String &album, int from, int to);  // positions in albumPhotos()

// "20240714-193205.pbm" → "14 Jul 2024"; "" for other names.
String dateTaken(const String &photoName);

}  // namespace album
