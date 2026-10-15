#include "facts_store.h"

#include <SD_MMC.h>
#include <esp_random.h>

#include <algorithm>

#include "log.h"
#include "storage.h"

// facts.txt, embedded by platformio.ini (board_build.embed_txtfiles; the end has a NUL).
extern const char kFactsStart[] asm("_binary_cartridges_facts_facts_txt_start");
extern const char kFactsEnd[] asm("_binary_cartridges_facts_facts_txt_end");

namespace facts {
namespace {

struct Fact {
  const char *text;  // in flash, not NUL-terminated
  uint16_t length;
  uint8_t category;
};

constexpr uint32_t kSeenMagic = 0x46534e31;  // "FSN1"

std::vector<Category> cats;
Fact *all = nullptr;  // PSRAM
size_t total = 0;
uint8_t *seen = nullptr;  // a bit per fact
uint32_t dataHash = 0;    // of facts.txt: seen.bin is for this text only
std::vector<uint16_t> favs;
bool sd = false;
size_t favouriteCursor = 0;

uint32_t fnv(const char *s, size_t n, uint32_t h = 2166136261u) {
  for (size_t i = 0; i < n; i++) h = (h ^ static_cast<uint8_t>(s[i])) * 16777619u;
  return h;
}

String path(const char *name) {
  return storage::myDataDir() + "/" + name;
}

bool isSeen(size_t i) {
  return seen[i / 8] & (1 << (i % 8));
}

void markSeen(size_t i, bool on) {
  if (on) seen[i / 8] |= 1 << (i % 8);
  else seen[i / 8] &= ~(1 << (i % 8));
}

void saveSeen() {
  if (!sd) return;
  File f = SD_MMC.open(path("seen.bin"), FILE_WRITE);
  if (!f) return;
  f.write(reinterpret_cast<const uint8_t *>(&kSeenMagic), 4);
  f.write(reinterpret_cast<const uint8_t *>(&dataHash), 4);
  f.write(seen, (total + 7) / 8);
}

void loadSeen() {
  File f = SD_MMC.open(path("seen.bin"));
  uint32_t magic = 0, hash = 0;
  if (!f || f.read(reinterpret_cast<uint8_t *>(&magic), 4) != 4 || f.read(reinterpret_cast<uint8_t *>(&hash), 4) != 4 ||
      magic != kSeenMagic || hash != dataHash) {
    return;  // none yet, or for other facts: start fresh
  }
  f.read(seen, (total + 7) / 8);
}

void saveFavourites() {
  if (!sd) return;
  File f = SD_MMC.open(path("favourites.txt"), FILE_WRITE);
  if (!f) return;
  for (uint16_t i : favs) f.printf("%08lx\n", static_cast<unsigned long>(id(i)));
}

void loadFavourites() {
  File f = SD_MMC.open(path("favourites.txt"));
  while (f && f.available()) {
    const String line = f.readStringUntil('\n');
    const int i = find(strtoul(line.c_str(), nullptr, 16));
    if (i >= 0 && std::find(favs.begin(), favs.end(), i) == favs.end()) favs.push_back(i);
  }
}

bool inScope(size_t i, int scope) {
  return scope == kAny || all[i].category == scope;
}

}  // namespace

bool begin() {
  const char *p = kFactsStart, *end = kFactsEnd;
  while (end > p && (end[-1] == '\0' || end[-1] == '\n')) end--;
  dataHash = fnv(p, end - p);
  // Count the lines first, then index them (into PSRAM: ~80 KB).
  size_t lines = 0;
  for (const char *q = p; q < end; q++) lines += *q == '\n';
  all = static_cast<Fact *>(ps_malloc((lines + 1) * sizeof(Fact)));
  if (!all) return false;
  while (p < end) {
    const char *eol = static_cast<const char *>(memchr(p, '\n', end - p));
    if (!eol) eol = end;
    const size_t n = eol - p;
    if (n && p[0] == '@') {  // "@key|Name"
      const String header(p + 1, n - 1);
      const int bar = header.indexOf('|');
      Category c;
      c.key = bar > 0 ? header.substring(0, bar) : header;
      c.name = bar > 0 ? header.substring(bar + 1) : header;
      c.first = total;
      cats.push_back(c);
    } else if (n && !cats.empty()) {
      all[total++] = {p, static_cast<uint16_t>(n), static_cast<uint8_t>(cats.size() - 1)};
      cats.back().count++;
    }
    p = eol + 1;
  }
  seen = static_cast<uint8_t *>(ps_calloc((total + 7) / 8, 1));
  sd = storage::available();
  if (sd) {
    storage::makeDirs(storage::myDataDir());
    loadSeen();
    loadFavourites();
  }
  LOGI("facts", "%u facts in %u categories, %u seen, %u favourites", static_cast<unsigned>(total),
       static_cast<unsigned>(cats.size()), static_cast<unsigned>(seenCount(kAny)), static_cast<unsigned>(favs.size()));
  return total > 0;
}

const std::vector<Category> &categories() {
  return cats;
}

size_t count() {
  return total;
}

String text(size_t i) {
  String s;
  if (i < total) s.concat(all[i].text, all[i].length);
  return s;
}

int categoryOf(size_t i) {
  return i < total ? all[i].category : -1;
}

uint32_t id(size_t i) {
  return i < total ? fnv(all[i].text, all[i].length) : 0;
}

int find(uint32_t wanted) {
  for (size_t i = 0; i < total; i++) {
    if (id(i) == wanted) return i;
  }
  return -1;
}

size_t scopeCount(int scope) {
  if (scope == kFavourites) return favs.size();
  if (scope == kAny) return total;
  return scope >= 0 && scope < static_cast<int>(cats.size()) ? cats[scope].count : 0;
}

size_t seenCount(int scope) {
  size_t n = 0;
  for (size_t i = 0; i < total; i++) n += inScope(i, scope) && isSeen(i);
  return n;
}

int next(int scope) {
  if (scope == kFavourites) {
    if (favs.empty()) return -1;
    return favs[favouriteCursor++ % favs.size()];
  }
  size_t unseen = 0;
  for (size_t i = 0; i < total; i++) unseen += inScope(i, scope) && !isSeen(i);
  if (unseen == 0) {  // all of it seen: start the scope over
    for (size_t i = 0; i < total; i++) {
      if (inScope(i, scope)) markSeen(i, false);
    }
    unseen = scopeCount(scope);
    if (unseen == 0) return -1;
  }
  size_t pick = esp_random() % unseen;
  for (size_t i = 0; i < total; i++) {
    if (!inScope(i, scope) || isSeen(i)) continue;
    if (pick-- == 0) {
      markSeen(i, true);
      saveSeen();
      return i;
    }
  }
  return -1;
}

bool isFavourite(size_t i) {
  return std::find(favs.begin(), favs.end(), i) != favs.end();
}

void setFavourite(size_t i, bool on) {
  if (i >= total || isFavourite(i) == on) return;
  if (on) favs.push_back(i);
  else favs.erase(std::remove(favs.begin(), favs.end(), i), favs.end());
  saveFavourites();
}

const std::vector<uint16_t> &favourites() {
  return favs;
}

int lockFact(const tm &now, std::function<bool(size_t)> fits) {
  if (!total) return -1;
  tm day = now;
  const uint32_t days = static_cast<uint32_t>(mktime(&day) / 86400);
  uint32_t slot = days * 288 + (now.tm_hour * 60 + now.tm_min) / 5;
  // A well-mixed start for the slot, then the next facts until one fits the lock screen.
  slot ^= slot >> 16;
  slot *= 0x7feb352d;
  slot ^= slot >> 15;
  for (size_t tries = 0; tries < 60; tries++) {
    const size_t i = (slot + tries * 7919) % total;
    if (fits(i)) return i;
  }
  return -1;
}

}  // namespace facts
