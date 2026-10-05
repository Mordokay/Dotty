#include "joke_store.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD_MMC.h>

#include <algorithm>

#include "log.h"
#include "net.h"
#include "storage.h"

namespace jokes {
namespace {

constexpr const char *kApi = "https://v2.jokeapi.dev";
constexpr int kBatch = 10;              // JokeAPI's maximum per request
constexpr int kMaxId = 2000;            // sanity cap on the id range it reports
constexpr size_t kMinJokes = 100;       // a download with fewer is treated as failed
constexpr uint32_t kRefreshSeconds = 7 * 24 * 3600;
constexpr uint32_t kRetryEmptyMs = 10 * 60 * 1000;
constexpr uint32_t kRetryMs = 60 * 60 * 1000;

const char *const kNames[kCategories] = {"Dark", "Programming", "Misc", "Pun", "Spooky", "Christmas"};

std::vector<Joke> all;           // sorted by id; texts point into `text`
char *text = nullptr;            // jokes.tsv in PSRAM
std::vector<Favourite> favs;

// Sync state, shared with the download task.
portMUX_TYPE syncLock = portMUX_INITIALIZER_UNLOCKED;
volatile bool syncRunning = false;
volatile bool syncFinished = false;
volatile int syncDone = 0, syncTotal = 0;
char syncError[64] = "";
uint32_t syncedAt = 0;
uint32_t lastAttempt = 0;
bool attempted = false;

String path(const char *name) {
  return storage::myDataDir() + "/" + name;
}

int categoryIndex(const char *name) {
  for (int i = 0; i < kCategories; i++) {
    if (strcasecmp(name, kNames[i]) == 0) return i;
  }
  return Misc;
}

// One line, no tabs: newlines and tabs in a joke become spaces.
String clean(const char *in) {
  String out = in ? in : "";
  out.replace('\t', ' ');
  out.replace('\r', ' ');
  out.replace('\n', ' ');
  while (out.indexOf("  ") >= 0) out.replace("  ", " ");
  out.trim();
  return out;
}

void loadCorpus() {
  all.clear();
  free(text);
  text = nullptr;
  File f = SD_MMC.open(path("jokes.tsv"));
  if (!f) return;
  const size_t size = f.size();
  text = static_cast<char *>(ps_malloc(size + 1));
  if (!text) return;
  f.read(reinterpret_cast<uint8_t *>(text), size);
  text[size] = '\0';
  for (char *line = text; line && *line;) {
    char *next = strchr(line, '\n');
    if (next) *next++ = '\0';
    char *fields[4] = {line, nullptr, nullptr, nullptr};
    for (int i = 1; i < 4 && fields[i - 1]; i++) {
      char *tab = strchr(fields[i - 1], '\t');
      if (tab) {
        *tab = '\0';
        fields[i] = tab + 1;
      }
    }
    if (fields[2]) {
      Joke j;
      j.id = atoi(fields[0]);
      j.category = categoryIndex(fields[1]);
      j.setup = fields[2];
      j.punchline = fields[3] ? fields[3] : "";
      all.push_back(j);
    }
    line = next;
  }
  std::sort(all.begin(), all.end(), [](const Joke &a, const Joke &b) { return a.id < b.id; });
}

Joke *find(uint16_t id) {
  const auto it = std::lower_bound(all.begin(), all.end(), id, [](const Joke &j, uint16_t v) { return j.id < v; });
  return it != all.end() && it->id == id ? &*it : nullptr;
}

void loadSeen() {
  File f = SD_MMC.open(path("seen.tsv"));
  while (f && f.available()) {
    const String line = f.readStringUntil('\n');
    const int tab = line.indexOf('\t');
    if (tab <= 0) continue;
    if (Joke *j = find(line.substring(0, tab).toInt())) j->seenAt = strtoul(line.c_str() + tab + 1, nullptr, 10);
  }
}

void saveSeen() {
  File f = SD_MMC.open(path("seen.tsv"), FILE_WRITE);
  if (!f) return;
  for (const Joke &j : all) {
    if (j.seenAt) f.printf("%u\t%lu\n", j.id, static_cast<unsigned long>(j.seenAt));
  }
}

void loadFavourites() {
  favs.clear();
  File f = SD_MMC.open(path("favourites.json"));
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f) != DeserializationError::Ok) return;
  for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
    favs.push_back({static_cast<uint16_t>(o["id"] | 0), static_cast<uint8_t>(categoryIndex(o["category"] | "Misc")),
                    String(o["setup"] | ""), String(o["punchline"] | ""), o["saved"] | 0u});
  }
}

void saveFavourites() {
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  for (const Favourite &f : favs) {
    JsonObject o = list.add<JsonObject>();
    o["id"] = f.id;
    o["category"] = kNames[f.category];
    o["setup"] = f.setup;
    o["punchline"] = f.punchline;
    o["saved"] = f.saved;
  }
  File out = SD_MMC.open(path("favourites.json"), FILE_WRITE);
  if (out) serializeJson(doc, out);
}

void setSyncError(const char *message) {
  portENTER_CRITICAL(&syncLock);
  strlcpy(syncError, message, sizeof(syncError));
  portEXIT_CRITICAL(&syncLock);
}

// Downloads every English joke into jokes.part, then swaps it in. Runs as its own task so
// Dotty stays responsive (~30 HTTPS requests).
void syncTask(void *) {
  String error;
  int written = 0;
  bool ok = net::connect(error);
  int maxId = 0;
  if (ok) {
    String body;
    JsonDocument info;
    if (net::getString(String(kApi) + "/info", body, error) && deserializeJson(info, body) == DeserializationError::Ok) {
      maxId = min<int>(info["jokes"]["idRange"]["en"][1] | 0, kMaxId);
    }
    if (maxId <= 0) {
      ok = false;
      if (error.isEmpty()) error = "JokeAPI didn't answer";
    }
  }
  File out;
  if (ok) {
    out = SD_MMC.open(path("jokes.part"), FILE_WRITE);
    ok = static_cast<bool>(out);
    if (!ok) error = "cannot write to the card";
  }
  if (ok) {
    syncTotal = maxId / kBatch + 1;
    for (int start = 0; start <= maxId && ok; start += kBatch) {
      const int end = min(start + kBatch - 1, maxId);
      String body;
      const String url = String(kApi) + "/joke/Any?lang=en&amount=" + kBatch + "&idRange=" + start + "-" + end;
      if (!net::getString(url, body, error)) {
        ok = false;  // the network went away: keep the jokes we have
        break;
      }
      JsonDocument doc;
      if (deserializeJson(doc, body) == DeserializationError::Ok && !(doc["error"] | false)) {
        // amount > 1 gives {"jokes": [...]}; a lone joke comes back as the object itself.
        JsonArrayConst list = doc["jokes"].as<JsonArrayConst>();
        auto write = [&](JsonObjectConst j) {
          const bool two = strcmp(j["type"] | "", "twopart") == 0;
          out.printf("%d\t%s\t%s\t%s\n", j["id"] | 0, j["category"] | "Misc",
                     clean(two ? (j["setup"] | "") : (j["joke"] | "")).c_str(),
                     two ? clean(j["delivery"] | "").c_str() : "");
          written++;
        };
        if (list.isNull()) write(doc.as<JsonObjectConst>());
        for (JsonObjectConst j : list) write(j);
      }
      syncDone = syncDone + 1;
    }
    out.close();
    if (ok && written < static_cast<int>(kMinJokes)) {
      ok = false;
      error = "only " + String(written) + " jokes came back";
    }
    if (ok) {
      SD_MMC.remove(path("jokes.tsv"));
      ok = SD_MMC.rename(path("jokes.part"), path("jokes.tsv"));
      if (!ok) error = "cannot write to the card";
    } else {
      SD_MMC.remove(path("jokes.part"));
    }
  }
  net::disconnect();
  if (ok) {
    syncedAt = time(nullptr);
    Preferences prefs;
    prefs.begin("jokes", false);
    prefs.putULong("synced", syncedAt);
    prefs.end();
    setSyncError("");
    LOGI("jokes", "downloaded %d jokes", written);
  } else {
    setSyncError(error.c_str());
    LOGW("jokes", "download failed: %s", error.c_str());
  }
  syncFinished = true;
  syncRunning = false;
  vTaskDelete(nullptr);
}

uint32_t mix(uint32_t x) {  // integer hash: neighbouring slots land far apart
  x ^= x >> 16;
  x *= 0x7feb352d;
  x ^= x >> 15;
  x *= 0x846ca68b;
  x ^= x >> 16;
  return x;
}

}  // namespace

const char *categoryName(int category) {
  return category >= 0 && category < kCategories ? kNames[category] : "Any";
}

bool begin() {
  storage::makeDirs(storage::myDataDir());
  loadCorpus();
  loadSeen();
  loadFavourites();
  Preferences prefs;
  prefs.begin("jokes", true);
  syncedAt = prefs.getULong("synced", 0);
  prefs.end();
  LOGI("jokes", "%u jokes, %u unseen, %u favourites", static_cast<unsigned>(all.size()),
       static_cast<unsigned>(unseen(kAny)), static_cast<unsigned>(favs.size()));
  return !all.empty();
}

size_t count() {
  return all.size();
}

size_t unseen(int category) {
  return std::count_if(all.begin(), all.end(), [&](const Joke &j) {
    return !j.seenAt && (category == kAny || j.category == category);
  });
}

const Joke *pick(int category) {
  std::vector<const Joke *> fresh;
  const Joke *oldest = nullptr;
  for (const Joke &j : all) {
    if (category != kAny && j.category != category) continue;
    if (!j.seenAt) fresh.push_back(&j);
    else if (!oldest || j.seenAt < oldest->seenAt) oldest = &j;
  }
  if (!fresh.empty()) return fresh[esp_random() % fresh.size()];
  return oldest;  // every joke seen: the one seen longest ago comes back
}

void markSeen(uint16_t id) {
  if (Joke *j = find(id)) {
    j->seenAt = max<uint32_t>(1, time(nullptr));
    saveSeen();
  }
}

void measureLockFit(std::function<bool(const Joke &)> fits) {
  size_t n = 0;
  for (Joke &j : all) n += (j.fitsLock = fits(j));
  LOGI("jokes", "%u jokes fit the lock screen", static_cast<unsigned>(n));
}

const Joke *lockJoke(const tm &now) {
  std::vector<const Joke *> fitting;
  for (const Joke &j : all) {
    if (j.fitsLock) fitting.push_back(&j);
  }
  if (fitting.empty()) return nullptr;
  tm day = now;
  const uint32_t days = static_cast<uint32_t>(mktime(&day) / 86400);
  const uint32_t slot = days * 288 + (now.tm_hour * 60 + now.tm_min) / 5;
  return fitting[mix(slot) % fitting.size()];
}

const std::vector<Favourite> &favourites() {
  return favs;
}

bool isFavourite(uint16_t id) {
  return std::any_of(favs.begin(), favs.end(), [&](const Favourite &f) { return f.id == id; });
}

void setFavourite(uint16_t id, uint8_t category, const String &setup, const String &punchline, bool on) {
  favs.erase(std::remove_if(favs.begin(), favs.end(), [&](const Favourite &f) { return f.id == id; }), favs.end());
  if (on) favs.insert(favs.begin(), {id, category, setup, punchline, static_cast<uint32_t>(time(nullptr))});
  saveFavourites();
}

bool syncDue() {
  if (syncRunning || net::saved().empty()) return false;
  const uint32_t since = millis() - lastAttempt;
  if (all.empty()) return !attempted || since >= kRetryEmptyMs;
  const uint32_t now = time(nullptr);
  return now - syncedAt >= kRefreshSeconds && (!attempted || since >= kRetryMs);
}

void startSync() {
  if (syncRunning) return;
  attempted = true;
  lastAttempt = millis();
  syncRunning = true;
  syncFinished = false;
  syncDone = 0;
  syncTotal = 0;
  LOGI("jokes", "downloading jokes");
  // HTTPS needs a deep stack.
  if (xTaskCreatePinnedToCore(syncTask, "jokes-sync", 12288, nullptr, 3, nullptr, 0) != pdPASS) {
    syncRunning = false;
    setSyncError("not enough memory");
  }
}

SyncState syncState() {
  SyncState s;
  s.running = syncRunning;
  s.done = syncDone;
  s.total = syncTotal;
  char error[sizeof(syncError)];
  portENTER_CRITICAL(&syncLock);
  memcpy(error, syncError, sizeof(error));  // no allocation inside the critical section
  portEXIT_CRITICAL(&syncLock);
  s.error = error;
  s.syncedAt = syncedAt;
  return s;
}

bool takeSyncFinished() {
  if (!syncFinished) return false;
  syncFinished = false;
  if (syncError[0] == '\0') {
    loadCorpus();
    loadSeen();
  }
  return true;
}

}  // namespace jokes
