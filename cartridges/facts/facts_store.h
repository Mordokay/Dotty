#pragma once

#include <Arduino.h>
#include <time.h>

#include <functional>
#include <vector>

// Ten thousand mind-blowing facts in 20 categories, built into the firmware (facts.txt, embedded
// by platformio.ini): "@key|Name" starts a category, then one fact per line (ASCII, <= 200
// characters, made with tools/build_facts.py). They never need a download or an update.
//
// On the card (the cartridge's data folder):
//   seen.bin        which facts were shown (a bit each), so new ones come first; reset per
//                   category once all of it was seen, and when facts.txt changes
//   favourites.txt  starred facts, one id per line (FNV-1a of the text, 8 hex digits)
namespace facts {

struct Category {
  String key, name;
  uint16_t first = 0, count = 0;  // its facts are first .. first + count - 1
};

constexpr int kAny = -1;         // every category ("Surprise me")
constexpr int kFavourites = -2;  // the starred ones

bool begin();  // indexes the facts, loads seen + favourites (after storage::begin)
const std::vector<Category> &categories();
size_t count();
String text(size_t i);
int categoryOf(size_t i);
uint32_t id(size_t i);
int find(uint32_t id);  // -1 if no fact has it

// The next fact for a scope (a category index, kAny or kFavourites): a random one not seen yet
// (favourites: the next in order), marked seen. -1 if the scope is empty.
int next(int scope);
size_t seenCount(int scope);
size_t scopeCount(int scope);

bool isFavourite(size_t i);
void setFavourite(size_t i, bool on);
const std::vector<uint16_t> &favourites();  // in the order they were starred

// The lock screen's fact for a 5-minute slot of the clock: one that `fits`.
int lockFact(const tm &now, std::function<bool(size_t)> fits);

}  // namespace facts
