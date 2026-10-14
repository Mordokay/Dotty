#include "updates.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include "cartridge.h"
#include "installer.h"
#include "library.h"
#include "log.h"
#include "net.h"

namespace updates {
namespace {

constexpr const char *kCatalogUrl = "https://github.com/Mordokay/Dotty/releases/latest/download/catalog.json";
constexpr uint32_t kEvery = 24 * 3600;      // seconds between checks
constexpr uint32_t kRetryMs = 3600 * 1000;  // after a check that failed
constexpr int kKeep = 2;                    // versions of each cartridge kept on the card
constexpr int kTries = 3;                   // per download (a weak signal broke one at -89 dBm)

std::mutex lock;  // guards `state` (the task writes, the main loop reads)
Status state;
volatile bool finished = false;
volatile bool cancel = false;
volatile bool keepWifi = false;
uint32_t failedAt = 0;  // millis() of the last check that failed (0 = none)

void setStep(const String &step, size_t size = 0) {
  std::lock_guard<std::mutex> guard(lock);
  state.step = step;
  state.bytes = 0;
  state.size = size;
}

// Internal RAM is what an HTTPS connection needs (~32 KB in one block for mbedTLS): with BLE
// connected only ~50 KB is free, and with the parsed catalog (~15 KB) still held there every
// download failed at once (ESP_ERR_HTTP_CONNECT, largest block 31.7 KB). So the catalog is read
// and parsed in PSRAM, and freed before the first download.
struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t size) override { return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM); }
};
constexpr size_t kMaxCatalog = 64 * 1024;

struct Plan {
  library::Entry entry;
  String url;
  const uint8_t *icon = nullptr;  // in `icons` (PSRAM), or none
};

String toHex(const uint8_t *digest) {
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return hex;
}

// Into <version>.part, checked against the catalog's size and SHA-256, then committed.
bool download(const Plan &plan, String &error) {
  const library::Entry &entry = plan.entry;
  const String path = library::tempPath(entry.id, entry.version);
  File out = SD_MMC.open(path, FILE_WRITE);
  if (!out) {
    error = "cannot write to the card";
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  size_t received = 0;
  const bool ok = net::download(
      plan.url,
      [&](const uint8_t *data, size_t len) {
        if (cancel) return false;
        mbedtls_sha256_update(&sha, data, len);
        received += len;
        {
          std::lock_guard<std::mutex> guard(lock);
          state.bytes = received;
        }
        return out.write(data, len) == len;
      },
      nullptr, error);
  out.close();
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (cancel) error = "stopped";
  else if (ok && received != entry.size) error = "download incomplete";
  else if (ok && toHex(digest) != entry.sha256) error = "download corrupted (SHA-256)";
  else if (!ok && error.isEmpty()) error = "card write failed";
  if (error.length()) {
    SD_MMC.remove(path);
    return false;
  }
  return library::commit(entry, plan.icon, error);
}

// Keeps the newest kKeep versions of every cartridge (not the launcher's: Rescue may need an
// older good one).
void prune() {
  std::vector<library::Entry> all = library::list();
  std::sort(all.begin(), all.end(), [](const library::Entry &a, const library::Entry &b) {
    return a.id != b.id ? a.id < b.id : library::isNewer(a.version, b.version);
  });
  int kept = 0;
  String id;
  for (const library::Entry &e : all) {
    if (e.id != id) id = e.id, kept = 0;
    if (e.id == "launcher") continue;
    if (++kept > kKeep) library::remove(e);
  }
}

void task(void *) {
  String error;
  int downloaded = 0, failed = 0;
  setStep("Joining Wi-Fi");
  bool ok = net::connect(error);
  std::vector<Plan> plans;
  uint8_t *icons = nullptr;  // the plans' icons, kIconBytes each (PSRAM)
  if (ok && !cancel) {
    setStep("Checking");
    char *text = static_cast<char *>(heap_caps_malloc(kMaxCatalog, MALLOC_CAP_SPIRAM));
    size_t length = 0;
    ok = text && net::download(
                     kCatalogUrl,
                     [&](const uint8_t *data, size_t n) {
                       if (length + n > kMaxCatalog) return false;
                       memcpy(text + length, data, n);
                       length += n;
                       return true;
                     },
                     nullptr, error);
    if (!text) error = "out of memory";
    PsramAllocator psram;
    JsonDocument catalog(&psram);
    if (ok && deserializeJson(catalog, text, length) != DeserializationError::Ok) {
      ok = false;
      error = "the catalog is unreadable";
    }
    heap_caps_free(text);
    if (ok && !cancel) {
      // What to fetch: a newer version of every cartridge on the card, and of the launcher.
      std::vector<String> onCard;
      for (const library::Entry &e : library::list()) {
        if (std::find(onCard.begin(), onCard.end(), e.id) == onCard.end()) onCard.push_back(e.id);
      }
      JsonArrayConst items = catalog["cartridges"].as<JsonArrayConst>();
      icons = static_cast<uint8_t *>(heap_caps_malloc(max<size_t>(1, items.size()) * installer::kIconBytes, MALLOC_CAP_SPIRAM));
      for (JsonObjectConst item : items) {
        Plan plan;
        library::Entry &entry = plan.entry;
        entry.id = item["id"] | "";
        entry.name = item["name"] | entry.id.c_str();
        entry.version = item["version"] | "";
        entry.size = item["size"] | 0;
        entry.sha256 = item["sha256"] | "";
        plan.url = item["firmware"] | "";
        if (entry.id.isEmpty() || entry.version.isEmpty() || !entry.size || plan.url.isEmpty()) continue;
        library::Entry newest;
        if (entry.id == "launcher") {
          if (!library::isNewer(entry.version, cartridge::self().version)) continue;
          if (library::find("launcher", entry.version, newest)) continue;  // already downloaded
        } else {
          if (std::find(onCard.begin(), onCard.end(), entry.id) == onCard.end()) continue;
          if (library::find(entry.id, "", newest) && !library::isNewer(entry.version, newest.version)) continue;
        }
        size_t iconLen = 0;
        const char *icon64 = item["icon"] | "";
        uint8_t *icon = icons ? icons + plans.size() * installer::kIconBytes : nullptr;
        if (icon && mbedtls_base64_decode(icon, installer::kIconBytes, &iconLen, reinterpret_cast<const uint8_t *>(icon64),
                                          strlen(icon64)) == 0 &&
            iconLen == installer::kIconBytes) {
          plan.icon = icon;
        }
        plans.push_back(plan);
      }
    }
  }  // the catalog is freed here, before any download
  const bool catalogRead = ok && !cancel;
  if (ok && !cancel) {
    {
      std::lock_guard<std::mutex> guard(lock);
      state.total = plans.size();
    }
    LOGI("updates", "%u newer versions in the catalog (internal heap %u free, largest block %u)",
         static_cast<unsigned>(plans.size()), heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    for (const Plan &plan : plans) {
      if (cancel) break;
      String why;
      bool got = false;
      for (int attempt = 1; attempt <= kTries && !got && !cancel; attempt++) {
        if (attempt > 1) {
          LOGW("updates", "%s: %s, trying again", plan.entry.name.c_str(), why.c_str());
          delay(5000);
          why = "";
        }
        setStep("Downloading " + plan.entry.name + " " + plan.entry.version, plan.entry.size);
        got = download(plan, why);
      }
      if (got) {
        downloaded++;
        LOGI("updates", "%s %s is on the card", plan.entry.id.c_str(), plan.entry.version.c_str());
      } else {
        failed++;
        error = plan.entry.name + ": " + why;
        LOGW("updates", "%s", error.c_str());
      }
      std::lock_guard<std::mutex> guard(lock);
      state.done++;
    }
    if (downloaded) prune();
    if (failed) ok = false;
  }
  heap_caps_free(icons);
  if (!keepWifi) net::disconnect();
  if (cancel) {
    ok = false;
    error = "stopped";
  }
  const uint32_t now = time(nullptr);
  if (ok) {
    Preferences p;
    p.begin("updates", false);
    p.putULong("checked", now);
    p.end();
  }
  LOGI("updates", "check %s: %d downloaded%s%s", ok ? "done" : "failed", downloaded, error.length() ? ", " : "",
       error.c_str());
  {
    std::lock_guard<std::mutex> guard(lock);
    state.running = false;
    state.step = "";
    state.error = ok ? String() : error;
    state.catalogRead = catalogRead;
    if (ok) state.checkedAt = now;
    state.downloaded = downloaded;
  }
  failedAt = ok || cancel ? 0 : max<uint32_t>(1, millis());
  finished = true;
  vTaskDelete(nullptr);
}

}  // namespace

void begin() {
  Preferences p;
  p.begin("updates", true);
  state.checkedAt = p.getULong("checked", 0);
  p.end();
}

bool due() {
  if (state.running || net::saved().empty() || !library::available()) return false;
  if (failedAt) return millis() - failedAt >= kRetryMs;
  const uint32_t now = time(nullptr);
  return !state.checkedAt || now < state.checkedAt || now - state.checkedAt >= kEvery;
}

void start() {
  {
    std::lock_guard<std::mutex> guard(lock);
    if (state.running) return;
    state.running = true;
    state.done = state.total = 0;
    state.error = "";
    state.step = "Starting";
  }
  cancel = false;
  keepWifi = false;
  finished = false;
  LOGI("updates", "checking for updates");
  if (xTaskCreatePinnedToCore(task, "updates", 12288, nullptr, 3, nullptr, 0) != pdPASS) {
    std::lock_guard<std::mutex> guard(lock);
    state.running = false;
    state.error = "out of memory";
  }
}

void stop(bool keep) {
  if (!state.running) return;
  keepWifi = keep;
  cancel = true;
  for (int i = 0; i < 300 && state.running; i++) delay(20);  // a chunk or a join: a few seconds at most
}

Status status() {
  std::lock_guard<std::mutex> guard(lock);
  return state;
}

bool takeFinished() {
  if (!finished) return false;
  finished = false;
  return true;
}

String newerLauncher() {
  library::Entry newest;
  if (!library::find("launcher", "", newest) || !library::isNewer(newest.version, cartridge::self().version)) return "";
  return newest.version;
}

}  // namespace updates
