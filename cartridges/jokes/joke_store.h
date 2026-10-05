#pragma once

#include <Arduino.h>
#include <time.h>

#include <functional>
#include <vector>

// Every English joke of JokeAPI (https://v2.jokeapi.dev, ~320, ~45 KB) kept on the SD card,
// so Dotty has jokes offline for as long as it likes. Files in the cartridge's data folder:
//   jokes.tsv       id \t category \t setup \t punchline (empty for one-liners), one per line
//   seen.tsv        id \t when it was last shown (local time, epoch seconds)
//   favourites.json [{id, category, setup, punchline, saved}] (kept even if JokeAPI drops one)
// A background task refreshes jokes.tsv over Wi-Fi: right away when it's empty, then weekly.
// No safe mode and no blacklist flags, on purpose.
namespace jokes {

enum Category : uint8_t { Dark, Programming, Misc, Pun, Spooky, Christmas, kCategories };
constexpr int kAny = -1;
const char *categoryName(int category);  // kAny → "Any"

struct Joke {
  uint16_t id = 0;
  uint8_t category = Misc;
  const char *setup = "";      // the whole joke for one-liners
  const char *punchline = "";  // empty for one-liners
  uint32_t seenAt = 0;         // 0 = never shown
  bool fitsLock = false;       // short enough for the lock screen
  bool twoPart() const { return punchline[0] != '\0'; }
};

struct Favourite {
  uint16_t id;
  uint8_t category;
  String setup, punchline;
  uint32_t saved;
};

bool begin();  // loads everything from the card (call after storage::begin)
size_t count();
size_t unseen(int category);

// The next joke for a category (kAny = all): a random unseen one, or else the one seen the
// longest ago (so a joke comes back once you've had time to forget it). nullptr if none.
const Joke *pick(int category);
void markSeen(uint16_t id);  // shown on Dotty's screen (the lock screen doesn't count)

// Works out which jokes fit the lock screen (needs the display's fonts).
void measureLockFit(std::function<bool(const Joke &)> fits);
// The lock screen's joke for this 5-minute slot: the same one all through 8:45-8:49 today,
// another at 8:50. nullptr if none fits.
const Joke *lockJoke(const tm &now);

// Favourites.
const std::vector<Favourite> &favourites();
bool isFavourite(uint16_t id);
void setFavourite(uint16_t id, uint8_t category, const String &setup, const String &punchline, bool on);

// Download, in the background.
struct SyncState {
  bool running = false;
  int done = 0, total = 0;  // requests
  String error;             // last failure ("" if the last run worked)
  uint32_t syncedAt = 0;    // local time of the last good download (0 = never)
};
bool syncDue();      // empty, or a week old (and not tried in the last hour)
void startSync();    // no-op while one is running
SyncState syncState();
// Call from the main loop: true once after a download finished (the jokes were reloaded).
bool takeSyncFinished();

}  // namespace jokes
