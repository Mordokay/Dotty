#include "memories.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

#include "log.h"
#include "storage.h"

namespace memories {
namespace {

MemoryStore store_;
bool available_ = false;
uint8_t chunk[1024];  // card reads and writes go through internal RAM, a piece at a time

// The save's JSON document goes in PSRAM: it runs mid-session, when TLS needs the internal RAM.
struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t size) override { return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM); }
} psram;

String dir() {
  return storage::myDataDir() + "/memory";
}

}  // namespace

void begin() {
  available_ = storage::available();
  if (!available_) return LOGW("memories", "no card: chats go on without memories");
  storage::makeDirs(dir());
  const String path = dir() + "/memories.json";
  File f = SD_MMC.open(path);
  if (!f) return LOGI("memories", "none yet");
  std::string json;
  json.reserve(f.size());
  for (int n; (n = f.read(chunk, sizeof(chunk))) > 0;) json.append(reinterpret_cast<const char *>(chunk), n);
  f.close();
  if (!store_.load(json)) {
    LOGW("memories", "memories.json doesn't parse: kept as memories.bad.json, starting empty");
    SD_MMC.remove(dir() + "/memories.bad.json");
    SD_MMC.rename(path, dir() + "/memories.bad.json");
    return;
  }
  LOGI("memories", "%u remembered", static_cast<unsigned>(store_.all().size()));
}

MemoryStore &store() {
  return store_;
}

bool available() {
  return available_;
}

bool save() {
  if (!available_) return false;
  const std::string json = store_.save(&psram);
  if (json.empty()) {  // out of memory: keep the file as it is rather than write one missing memories
    LOGW("memories", "couldn't build the file (out of memory): not saved");
    return false;
  }
  const String path = dir() + "/memories.json", part = path + ".part";
  File f = SD_MMC.open(part, FILE_WRITE);
  bool ok = static_cast<bool>(f);
  for (size_t at = 0; ok && at < json.size(); at += sizeof(chunk)) {
    const size_t n = min(sizeof(chunk), json.size() - at);
    memcpy(chunk, json.data() + at, n);
    ok = f.write(chunk, n) == n;
  }
  f.close();
  if (ok) {
    SD_MMC.remove(path);
    ok = SD_MMC.rename(part, path);
  }
  if (!ok) LOGW("memories", "couldn't save");
  return ok;
}

}  // namespace memories
