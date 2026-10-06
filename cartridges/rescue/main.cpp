// Rescue: Dotty's recovery firmware, in the factory partition. Flashed over USB and never
// updated by the app, so it stays a known way back. It runs only when asked to or when
// nothing else can (flash layout 2, partitions.csv):
//
//   - a launcher update: the launcher downloaded it to the SD card and set rescue/install
//     (NVS) to its version. Rescue writes it into the launcher slot (ota_1), checks it, and
//     starts it on trial: the new launcher must confirm itself (cartridge::confirmHealthy)
//     or the bootloader rolls it back.
//   - a launcher that failed its trial (the bootloader rolled it back, or a cartridge saw it
//     and handed over): Rescue puts the last good launcher (rescue/good, recorded by each
//     launcher once it has run fine; a copy is kept on the card) back and starts it.
//   - the bootloader found no bootable OTA slot: the same, or just start the launcher.
//   - a factory reset (rescue/reset, set by the launcher): format the SD card (FAT32), erase
//     every setting (the whole NVS partition: pairings, Wi-Fi, cartridge settings) and the
//     installed cartridge, keep the launcher (or install the newer one the app downloaded
//     first) and put its copy back on the fresh card, so Rescue still has a good one.
//
// No Bluetooth, no Wi-Fi, no touch: the e-paper says what's happening, then it restarts.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <driver/sdmmc_host.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_vfs_fat.h>
#include <mbedtls/sha256.h>
#include <nvs_flash.h>
#include <sdmmc_cmd.h>

#include "board_pins.h"
#include "cartridge.h"
#include "epd_display.h"
#include "log.h"
#include "ui.h"

DOTTY_CARTRIDGE("rescue", "Rescue", "1.2.0");

namespace {

constexpr const char *kFirstRescueLauncher = "0.9.0";  // older ones predate layout 2
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

EpdDisplay epd;

bool olderThan(const String &a, const String &b) {
  int x[3] = {}, y[3] = {};
  sscanf(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]);
  sscanf(b.c_str(), "%d.%d.%d", &y[0], &y[1], &y[2]);
  for (int i = 0; i < 3; i++) {
    if (x[i] != y[i]) return x[i] < y[i];
  }
  return false;
}

String launcherPath(const String &version) {
  return "/cartridges/launcher/firmware/" + version;
}

// "Dotty Rescue", a line saying what's happening, and a progress bar (percent < 0: none).
void show(const String &line1, const String &line2, int percent = -1, bool full = false) {
  epd.fillScreen(kWhite);
  epd.fillRect(0, 0, EpdDisplay::kSize, 45, kBlack);
  epd.setFont(&FreeSans9pt7b);
  epd.setTextColor(kWhite);
  ui::drawCentered(epd, "Dotty Rescue", 29);
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSansBold12pt7b);
  ui::drawWrapped(epd, line1, 92, EpdDisplay::kSize - 16, 2, 24);
  epd.setFont(&FreeSans9pt7b);
  ui::drawWrapped(epd, line2, 140, EpdDisplay::kSize - 16, 2, 20);
  if (percent >= 0) {
    epd.drawRect(15, 172, 170, 10, kBlack);
    epd.fillRect(17, 174, 166 * percent / 100, 6, kBlack);
  }
  if (full) epd.refreshFull();
  else epd.refreshPartial();
}

// Launcher versions on the card (>= kFirstRescueLauncher), newest first.
std::vector<String> launchersOnCard() {
  std::vector<String> versions;
  File dir = SD_MMC.open("/cartridges/launcher/firmware");
  for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
    String name = f.name();
    if (!name.endsWith(".bin")) continue;
    name.remove(name.length() - 4);
    if (!olderThan(name, kFirstRescueLauncher)) versions.push_back(name);
  }
  std::sort(versions.begin(), versions.end(), [](const String &a, const String &b) { return olderThan(b, a); });
  return versions;
}

// Writes /cartridges/launcher/firmware/<version>.bin into the launcher slot, checking it
// against its .json SHA-256, and selects it for the next boot (on trial).
bool installLauncher(const String &version, const char *what, String &error) {
  const esp_partition_t *slot = cartridge::launcherSlot();
  File meta = SD_MMC.open(launcherPath(version) + ".json");
  File in = SD_MMC.open(launcherPath(version) + ".bin");
  JsonDocument doc;
  if (!slot || !meta || !in || deserializeJson(doc, meta) != DeserializationError::Ok) {
    error = "launcher " + version + " isn't on the card";
    return false;
  }
  const String expected = doc["sha256"] | "";
  const size_t size = in.size();
  esp_ota_handle_t ota;
  if (esp_ota_begin(slot, size, &ota) != ESP_OK) {
    error = "can't write the launcher slot";
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  static uint8_t buffer[8192];
  size_t done = 0;
  int shown = -1;
  while (done < size) {
    const size_t n = in.read(buffer, min<size_t>(sizeof(buffer), size - done));
    if (n == 0 || esp_ota_write(ota, buffer, n) != ESP_OK) break;
    mbedtls_sha256_update(&sha, buffer, n);
    done += n;
    const int percent = done * 100 / size;
    if (percent / 10 != shown && !epd.isBusy()) {
      shown = percent / 10;
      show(what, "Launcher " + version, percent);
    }
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  if (done != size || !expected.equalsIgnoreCase(hex)) {
    esp_ota_abort(ota);
    error = "the copy on the card is damaged";
    return false;
  }
  if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(slot) != ESP_OK) {
    error = "the launcher doesn't verify";
    return false;
  }
  LOGI("rescue", "launcher %s installed", version.c_str());
  return true;
}

// ---------- factory reset ----------

String hexOf(const uint8_t *digest) {
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return hex;
}

// A launcher image held in PSRAM across the card format.
struct Image {
  uint8_t *data = nullptr;
  size_t size = 0;
  String version;
};

// The launcher in its slot (the one that asked for the reset: it runs fine).
bool copyInstalledLauncher(Image &out) {
  const esp_partition_t *slot = cartridge::launcherSlot();
  CartridgeInfo info;
  esp_image_metadata_t meta;
  const esp_partition_pos_t pos = {slot->address, slot->size};
  if (!cartridge::readLauncher(info) || esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) return false;
  out.data = static_cast<uint8_t *>(heap_caps_malloc(meta.image_len, MALLOC_CAP_SPIRAM));
  if (!out.data || esp_partition_read(slot, 0, out.data, meta.image_len) != ESP_OK) return false;
  out.size = meta.image_len;
  out.version = info.version;
  return true;
}

// A launcher file on the card (e.g. the newer one the app downloaded before the reset).
bool readLauncherFile(const String &version, Image &out) {
  File in = SD_MMC.open(launcherPath(version) + ".bin");
  if (!in) return false;
  out.data = static_cast<uint8_t *>(heap_caps_malloc(in.size(), MALLOC_CAP_SPIRAM));
  if (!out.data || in.read(out.data, in.size()) != in.size()) return false;
  out.size = in.size();
  out.version = version;
  return true;
}

// Writes /cartridges/launcher/firmware/<version>.bin + .json (as the launcher's library does).
bool writeLauncherFile(const Image &image) {
  SD_MMC.mkdir("/cartridges");
  SD_MMC.mkdir("/cartridges/launcher");
  SD_MMC.mkdir("/cartridges/launcher/firmware");
  File out = SD_MMC.open(launcherPath(image.version) + ".bin", FILE_WRITE);
  if (!out || out.write(image.data, image.size) != image.size) return false;
  out.close();
  uint8_t digest[32];
  mbedtls_sha256(image.data, image.size, digest, 0);
  JsonDocument meta;
  meta["id"] = "launcher";
  meta["name"] = "Launcher";
  meta["version"] = image.version;
  meta["size"] = image.size;
  meta["sha256"] = hexOf(digest);
  File json = SD_MMC.open(launcherPath(image.version) + ".json", FILE_WRITE);
  return json && serializeJson(meta, json) > 0;
}

// A fresh FAT32 file system (32 KB clusters, the usual for a card this size). Mounted through
// IDF directly: the Arduino wrapper doesn't expose the card handle the format call needs.
bool formatCard() {
  SD_MMC.end();
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slotConfig = SDMMC_SLOT_CONFIG_DEFAULT();
  slotConfig.width = 1;
  slotConfig.clk = static_cast<gpio_num_t>(PIN_SD_CLK);
  slotConfig.cmd = static_cast<gpio_num_t>(PIN_SD_CMD);
  slotConfig.d0 = static_cast<gpio_num_t>(PIN_SD_D0);
  slotConfig.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  esp_vfs_fat_mount_config_t config = {};
  config.format_if_mount_failed = true;  // a broken file system gets formatted too
  config.max_files = 4;
  config.allocation_unit_size = 32 * 1024;
  sdmmc_card_t *card = nullptr;
  esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slotConfig, &config, &card);
  if (err == ESP_OK) err = esp_vfs_fat_sdcard_format_cfg("/sdcard", card, &config);
  if (card) esp_vfs_fat_sdcard_unmount("/sdcard", card);
  LOGI("rescue", "card format: %s", esp_err_to_name(err));
  return err == ESP_OK && SD_MMC.begin("/sdcard", true);
}

[[noreturn]] void factoryReset(const String &newer) {
  show("Factory reset", "Erasing everything...", 0, true);
  Image good, fresh;
  const bool haveGood = copyInstalledLauncher(good);
  const bool haveFresh = newer.length() && newer != good.version && readLauncherFile(newer, fresh);
  LOGI("rescue", "factory reset: keeping launcher %s%s%s", haveGood ? good.version.c_str() : "?",
       haveFresh ? ", installing " : "", haveFresh ? newer.c_str() : "");

  show("Factory reset", "Formatting the SD card", 20);
  const bool formatted = formatCard();

  show("Factory reset", "Erasing settings and pairings", 50);
  nvs_flash_deinit();
  nvs_flash_erase();  // the whole NVS partition
  nvs_flash_init();
  const esp_partition_t *cartridgeSlot =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  if (cartridgeSlot) esp_partition_erase_range(cartridgeSlot, 0, 4096);  // no cartridge

  // The launcher(s) back on the card, so Rescue can always put one back.
  show("Factory reset", "Keeping the system", 70);
  Preferences prefs;
  prefs.begin("rescue", false);
  if (formatted && haveGood && writeLauncherFile(good)) prefs.putString("good", good.version);
  if (formatted && haveFresh && writeLauncherFile(fresh)) {
    String error;
    if (installLauncher(fresh.version, "Factory reset", error)) prefs.putString("trying", fresh.version);
  }
  prefs.end();
  show("Dotty is like new", formatted ? "Pair it again in the Dotty app" : "The SD card couldn't be formatted", -1, true);
  epd.waitBusy();
  delay(2000);
  esp_ota_set_boot_partition(cartridge::launcherSlot());
  esp_restart();
  for (;;) {
  }
}

void restartIntoLauncher() {
  epd.waitBusy();
  esp_ota_set_boot_partition(cartridge::launcherSlot());
  delay(100);
  esp_restart();
}

[[noreturn]] void stuck(const String &why) {
  LOGE("rescue", "%s", why.c_str());
  show("Dotty needs a computer", why + ". Flash the launcher over USB.", -1, true);
  epd.waitBusy();
  for (;;) delay(1000);
}

}  // namespace

void setup() {
  pinMode(PIN_VBAT_PWR, OUTPUT);
  digitalWrite(PIN_VBAT_PWR, HIGH);  // stay on when on battery
  Serial.begin(115200);
  dlog::begin();
  epd.begin();

  Preferences prefs;
  prefs.begin("rescue", false);
  const String staged = prefs.getString("install", "");
  const String good = prefs.getString("good", "");
  CartridgeInfo current;
  const bool haveLauncher = cartridge::readLauncher(current);
  const bool broken = cartridge::launcherBroken();
  LOGI("rescue", "staged '%s', good '%s', launcher %s%s", staged.c_str(), good.c_str(),
       haveLauncher ? current.version : "none", broken ? " (failed its trial)" : "");

  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  const bool card = SD_MMC.begin("/sdcard", true);

  // What happened, for the launcher to tell the app (rescue/result, resultVer, reason):
  // "failed" (couldn't install it) or "rolledBack" (it was installed but didn't start). An
  // update on trial is remembered as rescue/trying; the launcher turns it into "updated".
  auto record = [&](const char *result, const String &version, const String &reason) {
    prefs.putString("result", result);
    prefs.putString("resultVer", version);
    prefs.putString("reason", reason);
    prefs.remove("trying");
  };

  // 0. A factory reset (with the newer launcher the app may have downloaded first).
  if (prefs.getBool("reset", false)) {
    prefs.remove("reset");
    prefs.remove("install");
    prefs.end();
    if (!card) stuck("No SD card to reset");
    factoryReset(staged);
  }

  String error;
  // 1. An update the launcher left for us.
  if (staged.length()) {
    prefs.remove("install");  // once: a failing install must not loop
    if (!card) error = "no SD card";
    else if (installLauncher(staged, "Installing a new system", error)) {
      prefs.remove("result");
      prefs.putString("trying", staged);
      show("New system installed", "Starting launcher " + staged, -1);
      restartIntoLauncher();
    }
    LOGW("rescue", "update to %s failed: %s", staged.c_str(), error.c_str());
    record("failed", staged, error);
    if (!broken && haveLauncher) {  // the current launcher is untouched or fine: keep it
      show("Update failed", error + ". Keeping launcher " + String(current.version) + ".", -1);
      delay(3000);
      restartIntoLauncher();
    }
  }

  // 2. Nothing to repair: just start the launcher.
  if (!broken && haveLauncher) restartIntoLauncher();

  // 3. Put a good launcher back: the last one that ran fine, else the newest on the card
  // that isn't the one that just failed.
  if (!card) stuck("No SD card to take a good launcher from");
  std::vector<String> candidates;
  if (good.length() && !(haveLauncher && good == current.version && broken)) candidates.push_back(good);
  for (const String &v : launchersOnCard()) {
    if (std::find(candidates.begin(), candidates.end(), v) == candidates.end() &&
        !(haveLauncher && broken && v == current.version)) {
      candidates.push_back(v);
    }
  }
  if (good.length() && std::find(candidates.begin(), candidates.end(), good) == candidates.end()) {
    candidates.push_back(good);  // last resort: even the one that failed
  }
  const String failed = haveLauncher ? String(current.version) : String();
  for (const String &version : candidates) {
    if (installLauncher(version, "Putting back a working system", error)) {
      record("rolledBack", failed, "it didn't start, so Dotty went back to " + version);
      show("Dotty is fixed", "Starting launcher " + version, -1);
      restartIntoLauncher();
    }
    LOGW("rescue", "launcher %s: %s", version.c_str(), error.c_str());
  }
  stuck(candidates.empty() ? "No launcher on the SD card" : error);
}

void loop() {
  delay(1000);
}
