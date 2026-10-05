#include "storage.h"

#include <Preferences.h>
#include <SD_MMC.h>

#include "board_pins.h"
#include "cartridge.h"
#include "core_ble.h"
#include "log.h"

namespace storage {
namespace {

bool mounted = false;
std::function<void(const String &)> removeHook;
constexpr size_t kMaxNameBytes = 120;

bool isFirmwareFile(const String &name) {
  return name.endsWith(".bin") || name.endsWith(".json") || name.endsWith(".icon") || name.endsWith(".part");
}

// Older layouts: launcher copies lived directly in /cartridges/<id>/, songs in /music/.
void migrate() {
  File root = SD_MMC.open(kRoot);
  for (File dir = root ? root.openNextFile() : File(); dir; dir = root.openNextFile()) {
    if (!dir.isDirectory()) continue;
    const String id = dir.name();
    const String from = String(kRoot) + "/" + id;
    File folder = SD_MMC.open(from);
    for (File f = folder.openNextFile(); f; f = folder.openNextFile()) {
      const String name = f.name();
      if (f.isDirectory() || !isFirmwareFile(name)) continue;
      makeDirs(firmwareDir(id));
      f.close();
      SD_MMC.rename(from + "/" + name, firmwareDir(id) + "/" + name);
      LOGI("storage", "moved %s/%s into firmware/", id.c_str(), name.c_str());
    }
  }

  File oldMusic = SD_MMC.open("/music");
  if (!oldMusic || !oldMusic.isDirectory()) return;
  const String target = dataDir("music") + "/library";
  makeDirs(target);
  for (File f = oldMusic.openNextFile(); f; f = oldMusic.openNextFile()) {
    const String name = f.name();
    if (f.isDirectory() || name.startsWith(".")) continue;
    f.close();
    SD_MMC.rename("/music/" + name, target + "/" + name);
    LOGI("storage", "moved /music/%s to music/data/library", name.c_str());
  }
  oldMusic.close();
  removeTree("/music");
}

uint64_t treeBytes(const String &path) {
  File dir = SD_MMC.open(path);
  if (!dir || !dir.isDirectory()) return 0;
  uint64_t total = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) {
      const String child = path + "/" + f.name();
      f.close();
      total += treeBytes(child);
    } else {
      total += f.size();
    }
  }
  return total;
}

}  // namespace

std::vector<CartridgeFiles> cartridges() {
  std::vector<CartridgeFiles> out;
  if (!mounted) return out;
  File root = SD_MMC.open(kRoot);
  for (File dir = root ? root.openNextFile() : File(); dir; dir = root.openNextFile()) {
    if (!dir.isDirectory()) continue;
    CartridgeFiles c;
    c.id = dir.name();
    dir.close();
    File fw = SD_MMC.open(firmwareDir(c.id));
    for (File f = fw ? fw.openNextFile() : File(); f; f = fw.openNextFile()) {
      const String name = f.name();
      if (!f.isDirectory() && name.endsWith(".bin")) c.versions.push_back(name.substring(0, name.length() - 4));
    }
    c.dataBytes = treeBytes(dataDir(c.id));
    if (!c.versions.empty() || c.dataBytes > 0) out.push_back(c);
  }
  return out;
}

bool removeCartridge(const String &id, bool withData, String &error) {
  if (safeName(id) != id || id.isEmpty() || id == "launcher") {
    error = "not a cartridge";
    return false;
  }
  if (id == cartridge::self().id) {
    error = "switch to the launcher first";
    return false;
  }
  if (removeHook) removeHook(id);
  if (mounted) {
    removeTree(firmwareDir(id));
    if (withData) removeTree(cartridgeDir(id));
  }
  if (withData) {
    Preferences prefs;  // the cartridge's own settings (e.g. music/shuffle)
    if (prefs.begin(id.c_str(), false)) {
      prefs.clear();
      prefs.end();
    }
  }
  LOGI("storage", "removed %s (%s)", id.c_str(), withData ? "with its data" : "kept its data");
  return true;
}

void onRemove(std::function<void(const String &id)> hook) {
  removeHook = std::move(hook);
}

void registerCommands() {
  ble::on("storage.list", [](JsonObjectConst, JsonObject reply) {
    JsonArray list = reply["cartridges"].to<JsonArray>();
    for (const CartridgeFiles &c : cartridges()) {
      JsonObject o = list.add<JsonObject>();
      o["id"] = c.id;
      JsonArray versions = o["versions"].to<JsonArray>();
      for (const String &v : c.versions) versions.add(v);
      o["data"] = c.dataBytes;
    }
    if (mounted) reply["free"] = SD_MMC.totalBytes() - SD_MMC.usedBytes();
  });
  // {id, data}: data = true also deletes the cartridge's files and settings.
  ble::on("storage.remove", [](JsonObjectConst args, JsonObject reply) {
    String error;
    if (!removeCartridge(args["id"] | "", args["data"] | false, error)) {
      reply["ok"] = false;
      reply["error"] = error;
    }
  });
}

bool begin() {
  if (mounted) return true;
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  mounted = SD_MMC.begin("/sdcard", true);  // 1-bit bus
  if (!mounted) {
    LOGW("storage", "no SD card");
    return false;
  }
  makeDirs(kRoot);
  migrate();
  LOGI("storage", "%llu MB card, %llu MB used", SD_MMC.cardSize() >> 20, SD_MMC.usedBytes() >> 20);
  return true;
}

bool available() {
  return mounted;
}

String cartridgeDir(const String &id) {
  return String(kRoot) + "/" + id;
}

String firmwareDir(const String &id) {
  return cartridgeDir(id) + "/firmware";
}

String dataDir(const String &id) {
  return cartridgeDir(id) + "/data";
}

String myDataDir() {
  const String dir = dataDir(cartridge::self().id);
  makeDirs(dir);
  return dir;
}

void makeDirs(const String &path) {
  for (int i = 1; i <= static_cast<int>(path.length()); i++) {
    if (i == static_cast<int>(path.length()) || path[i] == '/') {
      const String part = path.substring(0, i);
      if (!SD_MMC.exists(part)) SD_MMC.mkdir(part);
    }
  }
}

bool removeTree(const String &path) {
  File dir = SD_MMC.open(path);
  if (!dir) return false;
  if (!dir.isDirectory()) {
    dir.close();
    return SD_MMC.remove(path);
  }
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const String child = path + "/" + f.name();
    f.close();
    removeTree(child);
  }
  dir.close();
  return SD_MMC.rmdir(path);
}

String safeName(const String &name) {
  String out;
  for (char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c < 32) continue;
    out += c;
  }
  out.trim();
  while (out.startsWith(".")) out.remove(0, 1);  // no "..", no hidden files
  if (out.length() > kMaxNameBytes) {
    // Shorten the stem, keep the extension (".mp3" decides what's a song), and never cut a
    // multi-byte UTF-8 character in half (FAT rejects invalid names).
    const int dot = out.lastIndexOf('.');
    const String ext = dot > 0 && out.length() - dot <= 8 ? out.substring(dot) : String();
    size_t keep = kMaxNameBytes - ext.length();
    while (keep > 0 && (static_cast<uint8_t>(out[keep]) & 0xC0) == 0x80) keep--;  // continuation byte
    out = out.substring(0, keep) + ext;
    out.trim();
  }
  return out;
}

}  // namespace storage
