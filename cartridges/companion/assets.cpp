#include "assets.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>

#include <string>

#include "asset_plan.h"
#include "avatar.h"
#include "avatar_pack.h"
#include "core_ble.h"
#include "log.h"
#include "net.h"
#include "power.h"
#include "storage.h"
#include "transfer.h"
#include "voice_pack.h"

namespace assets {
namespace {

bool ready[kAssetPackCount] = {};
AssetRetry retry[kAssetPackCount];
uint8_t *avatarData = nullptr;  // the PSRAM copy avatar::setPack draws from
bool changed = false;

// The task (one pack at a time) and what it hands over.
TaskHandle_t task = nullptr;
int current = -1;
volatile bool cancel = false, finished = false;
bool ok = false;
String error;
uint8_t *newAvatars = nullptr;
size_t avatarSize = 0, newAvatarSize = 0;
uint32_t startedAt = 0;

// HTTPS needs internal RAM (lwIP, the Wi-Fi driver; mbedTLS itself is in PSRAM on this build): below this,
// fail cleanly instead of letting an allocation deep in the system abort (it did, with Bluetooth on).
constexpr size_t kMinFree = 40 * 1024, kMinBlock = 20 * 1024;

bool enoughRam(String &err) {
  const size_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (free >= kMinFree && block >= kMinBlock) return true;
  err = "not enough memory (" + String(free / 1024) + " KB free, largest " + String(block / 1024) + " KB)";
  return false;
}

String dir() { return storage::myDataDir() + "/assets"; }
String stampPath(const AssetPack &p) { return dir() + "/" + p.stem + ".stamp"; }
String partPath(const AssetPack &p) { return dir() + "/" + p.stem + ".part"; }
String avatarsPath() { return storage::myDataDir() + "/avatars.bin"; }

std::string readStamp(const AssetPack &p) {
  File f = SD_MMC.open(stampPath(p));
  if (!f) return "";
  std::string s;
  while (f.available() && s.size() < 80) s += static_cast<char>(f.read());
  return s;
}

// The whole file into PSRAM when it's `size` bytes; nullptr otherwise.
uint8_t *readAll(const String &path, size_t size) {
  File f = SD_MMC.open(path);
  if (!f || f.size() != size) return nullptr;
  uint8_t *data = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
  if (data && f.read(data, size) != size) {
    free(data);
    data = nullptr;
  }
  return data;
}

// data/avatars.bin into PSRAM, whatever pack it is (an older firmware's keeps drawing until the new one is
// verified); nullptr when there's none or it isn't a valid DAV1 file.
uint8_t *loadAvatars(size_t &size) {
  File f = SD_MMC.open(avatarsPath());
  size = f ? f.size() : 0;
  f.close();
  uint8_t *data = size ? readAll(avatarsPath(), size) : nullptr;
  if (data && !AvatarPack(data, size).valid()) {
    free(data);
    data = nullptr;
  }
  return data;
}

String hex(const uint8_t *digest) {
  char out[65];
  for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
  return out;
}

bool download(const AssetPack &p, const char *base, String &err) {
  const String part = partPath(p);
  File out = SD_MMC.open(part, FILE_WRITE);
  if (!out) {
    err = "cannot write to the card";
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  size_t received = 0;
  const std::string url = assetUrl(base, p);
  const bool fetched = net::download(
      url.c_str(),
      [&](const uint8_t *data, size_t len) {
        if (cancel || ble::running()) return false;  // Bluetooth came on (unlocked): its RAM, its radio
        mbedtls_sha256_update(&sha, data, len);
        received += len;
        return out.write(data, len) == len;
      },
      nullptr, err);
  out.close();
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (cancel) err = "cancelled";
  else if (ble::running()) err = "Bluetooth came on";
  else if (fetched && received != p.size) err = "incomplete (" + String(received) + " bytes)";
  else if (fetched && hex(digest) != p.sha256) err = "corrupted (SHA-256)";
  else if (!fetched && err.isEmpty()) err = "card write failed";
  if (err.length()) {
    SD_MMC.remove(part);
    return false;
  }
  return true;
}

// The voice pack's samples into data/voices/<name>.mp3 (each via .part), then the pack goes.
bool unpackVoices(const AssetPack &p, String &err) {
  const String part = partPath(p);
  uint8_t *data = readAll(part, p.size);
  SD_MMC.remove(part);
  if (!data) {
    err = "couldn't read the pack back";
    return false;
  }
  const VoicePack pack(data, p.size);
  const String voices = storage::myDataDir() + "/voices";
  SD_MMC.mkdir(voices);
  bool good = pack.valid();
  for (int i = 0; good && i < pack.count() && !cancel; i++) {
    const String name = pack.name(i);
    const uint8_t *bytes = nullptr;
    size_t len = 0;
    const String path = voices + "/" + name + ".mp3";
    File f = SD_MMC.open(path + ".part", FILE_WRITE);
    good = f && pack.find(name.c_str(), bytes, len) && f.write(bytes, len) == len;  // from PSRAM: fine
    f.close();
    SD_MMC.remove(path);
    good = good && SD_MMC.rename(path + ".part", path);
  }
  free(data);
  if (cancel) good = false;  // stopped part-way: some samples are missing, so no stamp
  if (!good) err = cancel ? "cancelled" : "couldn't unpack the voices";
  return good;
}

bool install(const AssetPack &p, String &err) {
  if (!strcmp(p.stem, "avatars")) {
    SD_MMC.remove(avatarsPath());
    if (!SD_MMC.rename(partPath(p), avatarsPath())) {
      err = "couldn't rename the pack";
      return false;
    }
    newAvatars = loadAvatars(newAvatarSize);
    if (!newAvatars || newAvatarSize != p.size) {
      err = "the pack didn't load";
      return false;
    }
  } else if (!strcmp(p.stem, "voices")) {
    if (!unpackVoices(p, err)) return false;
  }
  if (cancel) {
    err = "cancelled";
    return false;
  }
  File f = SD_MMC.open(stampPath(p), FILE_WRITE);  // only now: the pack is in place
  const bool stamped = f && f.print(p.sha256) == strlen(p.sha256);
  f.close();
  if (!stamped) err = "couldn't write the stamp";
  return stamped;
}

void run(void *) {
  const AssetPack &p = kAssetPacks[current];
  String err;
  const bool joined = !net::connected();
  bool good = cancel ? false : (net::connected() || net::connect(err));
  if (cancel) good = false;  // connect() clears a stop's abortConnect() when it begins
  if (good) good = enoughRam(err) && download(p, kReleaseAssetBase, err) && !cancel && install(p, err);
  if (cancel && err.isEmpty()) err = "cancelled";
  if (joined && !transfer::active()) net::disconnect();  // only the Wi-Fi this download joined
  error = err;
  ok = good;
  finished = true;
  task = nullptr;
  vTaskDelete(nullptr);
}

const char *why = "";

}  // namespace

void begin() {
  SD_MMC.mkdir(dir());
  for (int i = 0; i < kAssetPackCount; i++) {
    const AssetPack &p = kAssetPacks[i];
    ready[i] = assetReady(readStamp(p), p);
    if (!strcmp(p.stem, "avatars")) {
      avatarData = loadAvatars(avatarSize);
      if (avatarData) avatar::setPack(avatarData, avatarSize);  // an older pack draws until the new one is in
      ready[i] = ready[i] && avatarData && avatarSize == p.size;
    }
    if (ready[i]) retry[i].succeeded();
    LOGI("assets", "%s %s", p.stem, ready[i] ? "ready" : "missing");
  }
}

void poll(bool idle) {
  if (finished) {  // the task ended: take its result over (here, on the loop)
    finished = false;
    const AssetPack &p = kAssetPacks[current];
    power::setWakeLock(power::kWakeLockDownload, false);
    if (ok) {
      ready[current] = true;
      retry[current].succeeded();
      if (newAvatars) {
        avatar::setPack(newAvatars, newAvatarSize);
        free(avatarData);
        avatarData = newAvatars;
        avatarSize = newAvatarSize;
        newAvatars = nullptr;
        changed = true;
      }
      LOGI("assets", "%s done in %lu s", p.stem, (millis() - startedAt) / 1000);
    } else {
      free(newAvatars);
      newAvatars = nullptr;
      if (cancel) retry[current].reset(millis());  // stopped for something else: try again soon (when idle)
      else retry[current].failed(millis());
      LOGW("assets", "%s failed: %s%s%s; next try in %lu s", p.stem, error.c_str(), *why ? " " : "", why,
           (retry[current].due() - millis()) / 1000);
    }
    current = -1;
    why = "";
  }
  if (task) {
    if (!idle && !cancel) {  // something else started: it gets the radio
      why = "(busy)";
      cancel = true;
    }
    return;
  }
  if (!idle) return;
  for (int i = 0; i < kAssetPackCount; i++) {
    if (ready[i] || !retry[i].isDue(millis())) continue;
    current = i;
    cancel = false;
    ok = false;
    error = "";
    startedAt = millis();
    power::setWakeLock(power::kWakeLockDownload, true);  // a locked Dotty stays awake for it
    LOGI("assets", "downloading %s (%lu bytes)", kAssetPacks[i].stem, static_cast<unsigned long>(kAssetPacks[i].size));
    if (xTaskCreatePinnedToCore(run, "assets", 12288, nullptr, 1, &task, 0) != pdPASS) {
      task = nullptr;
      error = "no memory for the task";
      finished = true;  // the next pass reports it
    }
    return;
  }
}

void stop(const char *reason) {
  if (!task) return;
  why = reason;
  cancel = true;
  net::abortConnect();
  // Until the task has really ended (its Wi-Fi off): Bluetooth or a chat starting under it was the crash. The
  // sink stops at its next block (well under a second while data flows); a TLS handshake or a stalled read
  // can take longer, so up to 20 s (the loop watchdog allows 90).
  for (int i = 0; i < 1000 && task; i++) vTaskDelay(pdMS_TO_TICKS(20));
  if (task) LOGW("assets", "the download is still ending after 20 s");
  poll(false);  // report it now, so the caller's Wi-Fi is its own
}

bool busy() { return task != nullptr; }

bool takeChanged() {
  const bool was = changed;
  changed = false;
  return was;
}

const char *state(const char *stem) {
  for (int i = 0; i < kAssetPackCount; i++) {
    if (strcmp(kAssetPacks[i].stem, stem)) continue;
    if (ready[i]) return "ready";
    return current == i ? "downloading" : "missing";
  }
  return "missing";
}

bool voicesReady() { return !strcmp(state("voices"), "ready"); }

}  // namespace assets
