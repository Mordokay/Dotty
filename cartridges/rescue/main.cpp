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
//
// No Bluetooth, no Wi-Fi, no touch: the e-paper says what's happening, then it restarts.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>

#include "board_pins.h"
#include "cartridge.h"
#include "epd_display.h"
#include "log.h"
#include "ui.h"

DOTTY_CARTRIDGE("rescue", "Rescue", "1.0.0");

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

  String error;
  // 1. An update the launcher left for us.
  if (staged.length()) {
    prefs.remove("install");  // once: a failing install must not loop
    if (!card) error = "no SD card";
    else if (installLauncher(staged, "Installing a new system", error)) {
      show("New system installed", "Starting launcher " + staged, -1);
      restartIntoLauncher();
    }
    LOGW("rescue", "update to %s failed: %s", staged.c_str(), error.c_str());
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
  for (const String &version : candidates) {
    if (installLauncher(version, "Putting back a working system", error)) {
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
