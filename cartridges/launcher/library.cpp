#include "library.h"

#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>

#include "board_pins.h"
#include "installer.h"
#include "log.h"

namespace library {
namespace {

constexpr const char *kRoot = "/cartridges";
constexpr size_t kChunk = 8192;

bool mounted = false;

String basePath(const String &id, const String &version) {
  return String(kRoot) + "/" + id + "/" + version;
}

String toHex(const uint8_t *digest) {
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return hex;
}

bool readEntry(const String &jsonPath, Entry &out) {
  File f = SD_MMC.open(jsonPath);
  if (!f) return false;
  JsonDocument doc;
  if (deserializeJson(doc, f) != DeserializationError::Ok) return false;
  out.id = doc["id"] | "";
  out.name = doc["name"] | "";
  out.version = doc["version"] | "";
  out.sha256 = doc["sha256"] | "";
  out.size = doc["size"] | 0;
  out.hasIcon = SD_MMC.exists(basePath(out.id, out.version) + ".icon");
  return out.id.length() && out.version.length() && out.size > 0 && out.sha256.length() == 64;
}

// Simple "newer" ordering for versions like 0.5.0 vs 0.10.0.
bool newer(const String &a, const String &b) {
  int ai[3] = {}, bi[3] = {};
  sscanf(a.c_str(), "%d.%d.%d", &ai[0], &ai[1], &ai[2]);
  sscanf(b.c_str(), "%d.%d.%d", &bi[0], &bi[1], &bi[2]);
  for (int i = 0; i < 3; i++) {
    if (ai[i] != bi[i]) return ai[i] > bi[i];
  }
  return false;
}

void writeMeta(const Entry &entry, const uint8_t *icon) {
  const String base = basePath(entry.id, entry.version);
  JsonDocument doc;
  doc["id"] = entry.id;
  doc["name"] = entry.name;
  doc["version"] = entry.version;
  doc["size"] = entry.size;
  doc["sha256"] = entry.sha256;
  File meta = SD_MMC.open(base + ".json", FILE_WRITE);
  serializeJson(doc, meta);
  meta.close();
  if (icon) {
    File iconFile = SD_MMC.open(base + ".icon", FILE_WRITE);
    iconFile.write(icon, installer::kIconBytes);
  }
}

const esp_partition_t *slot() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
}

}  // namespace

bool begin() {
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  mounted = SD_MMC.begin("/sdcard", true);  // 1-bit bus
  if (!mounted) {
    LOGW("library", "no SD card");
    return false;
  }
  if (!SD_MMC.exists(kRoot)) SD_MMC.mkdir(kRoot);
  LOGI("library", "%u cartridges on the card", static_cast<unsigned>(list().size()));
  return true;
}

bool available() {
  return mounted;
}

std::vector<Entry> list() {
  std::vector<Entry> entries;
  if (!mounted) return entries;
  File root = SD_MMC.open(kRoot);
  if (!root) return entries;
  for (File dir = root.openNextFile(); dir; dir = root.openNextFile()) {
    if (!dir.isDirectory()) continue;
    const String dirPath = String(kRoot) + "/" + dir.name();
    File folder = SD_MMC.open(dirPath);
    for (File f = folder.openNextFile(); f; f = folder.openNextFile()) {
      const String name = f.name();
      Entry e;
      if (name.endsWith(".json") && readEntry(dirPath + "/" + name, e)) entries.push_back(e);
    }
  }
  return entries;
}

bool find(const String &id, const String &version, Entry &out) {
  bool found = false;
  for (const Entry &e : list()) {
    if (e.id != id) continue;
    if (version.length() ? e.version == version : (!found || newer(e.version, out.version))) {
      out = e;
      found = true;
    }
  }
  return found;
}

bool readIcon(const Entry &entry, uint8_t *out) {
  File f = SD_MMC.open(basePath(entry.id, entry.version) + ".icon");
  return f && f.read(out, installer::kIconBytes) == installer::kIconBytes;
}

bool saveInstalled(const Entry &entry, const uint8_t *icon, String &error) {
  if (!mounted) {
    error = "no SD card";
    return false;
  }
  const esp_partition_t *part = slot();
  SD_MMC.mkdir(String(kRoot) + "/" + entry.id);
  const String base = basePath(entry.id, entry.version);
  File out = SD_MMC.open(base + ".bin", FILE_WRITE);
  if (!part || !out) {
    error = "cannot write to the card";
    return false;
  }

  static uint8_t buffer[kChunk];
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  for (size_t done = 0; done < entry.size;) {
    const size_t n = min(kChunk, entry.size - done);
    if (esp_partition_read(part, done, buffer, n) != ESP_OK || out.write(buffer, n) != n) {
      error = "card write failed";
      break;
    }
    mbedtls_sha256_update(&sha, buffer, n);
    done += n;
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  out.close();
  if (error.isEmpty() && toHex(digest) != entry.sha256) error = "copy does not match";
  if (error.length()) {
    SD_MMC.remove(base + ".bin");
    return false;
  }

  writeMeta(entry, icon);
  LOGI("library", "saved %s %s to the card", entry.id.c_str(), entry.version.c_str());
  return true;
}

String tempPath(const String &id, const String &version) {
  SD_MMC.mkdir(String(kRoot) + "/" + id);
  return basePath(id, version) + ".part";
}

bool commit(const Entry &entry, const uint8_t *icon, String &error) {
  const String base = basePath(entry.id, entry.version);
  SD_MMC.remove(base + ".bin");
  if (!SD_MMC.rename(base + ".part", base + ".bin")) {
    error = "cannot save on the card";
    return false;
  }
  writeMeta(entry, icon);
  LOGI("library", "stored %s %s on the card", entry.id.c_str(), entry.version.c_str());
  return true;
}

bool install(const Entry &entry, Progress progress, String &error) {
  const esp_partition_t *part = slot();
  File in = SD_MMC.open(basePath(entry.id, entry.version) + ".bin");
  if (!part || !in || in.size() != entry.size) {
    error = "cartridge file missing on the card";
    return false;
  }
  // The size is known, so erase the whole region up front: large block erases are far
  // faster than the 4 KB sector-by-sector erasing of OTA_WITH_SEQUENTIAL_WRITES.
  esp_ota_handle_t ota;
  if (esp_ota_begin(part, entry.size, &ota) != ESP_OK) {
    error = "flash busy";
    return false;
  }

  static uint8_t buffer[kChunk];
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  const uint32_t start = millis();
  for (size_t done = 0; done < entry.size;) {
    const size_t n = in.read(buffer, min(kChunk, entry.size - done));
    if (n == 0 || esp_ota_write(ota, buffer, n) != ESP_OK) {
      error = "copy failed";
      break;
    }
    mbedtls_sha256_update(&sha, buffer, n);
    done += n;
    if (progress) progress(done, entry.size);
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (error.isEmpty() && toHex(digest) != entry.sha256) error = "card copy is corrupted";
  if (error.length()) {
    esp_ota_abort(ota);
    return false;
  }
  if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
    error = "not a valid firmware image";
    return false;
  }
  LOGI("library", "installed %s %s from the card in %lu ms", entry.id.c_str(), entry.version.c_str(),
       millis() - start);
  return true;
}

}  // namespace library
