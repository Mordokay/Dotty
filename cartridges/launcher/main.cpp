// Launcher: the permanent firmware in the factory partition. Shows which cartridge is
// installed and starts it, installs cartridges (over BLE, or from the SD card library),
// and keeps a copy of each installed cartridge on the card. Reached at boot when no
// valid cartridge is installed, or from a cartridge with BOOT + PWR / core.toLauncher.

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <SD_MMC.h>
#include <mbedtls/base64.h>
#include <Preferences.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include "art.h"
#include "battery.h"
#include "board_pins.h"
#include "cartridge.h"
#include "core_ble.h"
#include "images/firefly.h"
#include "installer.h"
#include "library.h"
#include "log.h"
#include "net.h"
#include "storage.h"
#include "transfer.h"
#include "shell.h"
#include "ui.h"

DOTTY_CARTRIDGE("launcher", "Launcher", "0.9.9");

namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;
constexpr const char *kCatalogUrl = "https://github.com/Mordokay/Dotty/releases/latest/download/catalog.json";

CartridgeInfo installed;
bool hasCartridge = false;
bool startRequested = false;  // set by the launcher.start command

// Factory reset (Rescue does it): asked on screen after holding BOOT 10 s on the home screen,
// or requested by the app (launcher.factoryReset).
constexpr uint32_t kResetHoldMs = 10000;
constexpr uint32_t kResetAskMs = 20000;  // the question goes away by itself
uint32_t askResetUntil = 0;
bool resetRequested = false;
String resetNewer;

// What the progress screen shows: a BLE install, an install from the card, a download.
// Restarting / failing is acted on in loop(), after the command's reply has been sent.
enum class Stage { None, Running, Done, Failed };
struct Job {
  Stage stage = Stage::None;
  bool fromBle = false;  // progress comes from the BLE installer
  bool rescue = false;   // a launcher update: Done restarts into Rescue, which installs it
  String action;         // "Installing", "Installing from card", …
  String name, version, error;
  size_t size = 0, done = 0;
  uint8_t icon[installer::kIconBytes];
  bool hasIcon = false;
  bool failureShown = false;
};
Job job;

// The last launcher update's outcome, from Rescue's notes (rescue/result…): "updated",
// "failed" or "rolledBack", until the app has shown it (launcher.updateSeen).
struct UpdateResult {
  String status, version, reason;
} lastUpdate;

void loadUpdateResult() {
  Preferences p;
  p.begin("rescue", true);
  lastUpdate.status = p.getString("result", "");
  lastUpdate.version = p.getString("resultVer", "");
  lastUpdate.reason = p.getString("reason", "");
  p.end();
}

bool updateFailed() {
  return lastUpdate.status == "failed" || lastUpdate.status == "rolledBack";
}

// ---------- welcome (first steps after switching on, or a factory reset) ----------

// Where setting Dotty up has got to: paired with a phone, on Wi-Fi, with a cartridge.
enum class Setup { Welcome, WiFi, Cartridge, Ready };

int previewStage = -1;  // developer aid (serial key 'w'): show a welcome step as if it were real

Setup setupStage() {
  if (previewStage >= 0) return static_cast<Setup>(previewStage);
  if (ble::bondCount() == 0) return Setup::Welcome;
  if (net::saved().empty()) return Setup::WiFi;
  if (!hasCartridge) return Setup::Cartridge;
  return Setup::Ready;
}

String joiningSsid;           // wifi.add is joining this network right now
String joinFailedSsid;        // …and couldn't: said for a moment
uint32_t joinFailedUntil = 0;

// The firefly, a friendly title and two short lines: no jargon.
void drawGreeting(const String &title, const String &line1, const String &line2) {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  epd.drawBitmap((kW - kFireflyWidth) / 2, 2, kFirefly, kFireflyWidth, kFireflyHeight, kBlack);
  ui::drawBattery(epd, kW - 8 - 29, 8, battery::percent(), battery::charging());
  epd.setFont(&FreeSansBold12pt7b);
  ui::drawCentered(epd, title, 146);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, ui::fitText(epd, line1, kW - 8), 170);
  ui::drawCentered(epd, ui::fitText(epd, line2, kW - 8), 190);
}

// True if a welcome step took the home screen.
bool drawWelcome() {
  if (joiningSsid.length()) {
    drawGreeting("One moment...", "Joining " + ui::printable(joiningSsid), "");
    return true;
  }
  if (joinFailedUntil && millis() < joinFailedUntil) {
    drawGreeting("Hmm, no luck", "Couldn't join " + ui::printable(joinFailedSsid), "Check the password in the app");
    return true;
  }
  switch (setupStage()) {
    case Setup::Welcome:
      drawGreeting("Hi, I'm Dotty!", "Open the Dotty app", "and pick " + ble::name());
      return true;
    case Setup::WiFi:
      drawGreeting("We're friends!", "Next, pick a Wi-Fi", "network in the app");
      return true;
    case Setup::Cartridge:
      drawGreeting("All set!", "Choose a cartridge", "in the Dotty app");
      return true;
    default:
      return false;
  }
}

void drawHome(const char *hint = nullptr) {
  if (!hint && drawWelcome()) return;
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  epd.drawBitmap((kW - kFireflyWidth) / 2, 2, kFirefly, kFireflyWidth, kFireflyHeight, kBlack);
  ui::drawBattery(epd, kW - 8 - 29, 8, battery::percent(), battery::charging());

  epd.setFont(&FreeSansBold12pt7b);
  ui::drawCentered(epd, "Dotty", 144);

  epd.setFont(&FreeSans9pt7b);
  if (hasCartridge) {
    ui::drawCentered(epd, ui::fitText(epd, String(installed.name) + " is ready", kW - 16), 168);
  } else {
    ui::drawCentered(epd, "No cartridge yet", 168);
  }
  if (hint) {
    ui::drawCentered(epd, hint, 192);
  } else if (updateFailed()) {
    ui::drawCentered(epd, ui::fitText(epd, "Update to " + lastUpdate.version + " failed", kW - 8), 192);
  } else if (hasCartridge) {
    ui::drawCentered(epd, "Press BOOT to start", 192);
  }
}

void startCartridge() {
  startRequested = false;
  const String hint = String("Starting ") + installed.name + "...";
  drawHome(hint.c_str());
  shell::refresh(false);
  epd.waitBusy();
  cartridge::startInstalled();  // restarts the chip on success
  drawHome("Could not start it");
  shell::refresh(false);
}

// ---------- progress screen ----------

void drawJob() {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);

  const int16_t iconX = (kW - installer::kIconSize) / 2, iconY = 8;
  if (job.hasIcon) {
    epd.drawBitmap(iconX, iconY, job.icon, installer::kIconSize, installer::kIconSize, kBlack);
  } else {
    // No icon yet (e.g. connecting to Wi-Fi before the catalog arrives).
    art::drawWifi(epd, iconX, iconY, installer::kIconSize);
  }

  epd.setFont(&FreeSans9pt7b);
  const String stage = job.stage == Stage::Done     ? String("Starting")
                       : job.stage == Stage::Failed ? String("Failed:")
                                                    : job.action;
  ui::drawCentered(epd, stage, 96);
  epd.setFont(&FreeSansBold12pt7b);
  ui::drawCentered(epd, ui::fitText(epd, job.stage == Stage::Failed ? job.error : job.name, kW - 16), 122);

  const size_t done = job.stage == Stage::Done ? job.size : job.done;
  const int percent = job.size ? static_cast<int>(done * 100 / job.size) : 0;
  const int16_t barX = 20, barY = 138, barW = kW - 40, barH = 12;
  epd.drawRoundRect(barX, barY, barW, barH, 6, kBlack);
  epd.fillRoundRect(barX + 2, barY + 2, (barW - 4) * percent / 100, barH - 4, 4, kBlack);

  // Size and version are unknown at first while fetching (until the catalog arrives).
  epd.setFont(&FreeSans9pt7b);
  if (job.size > 0) {
    char line[40];
    snprintf(line, sizeof(line), "%d%%  %u / %u KB", percent, static_cast<unsigned>(done / 1024),
             static_cast<unsigned>(job.size / 1024));
    ui::drawCentered(epd, line, 172);
  }
  if (job.version.length()) ui::drawCentered(epd, "v" + job.version, 194);
}

// Partial refresh about once a second while the percentage moves.
void updateJobScreen() {
  static int shownPercent = -1;
  static uint32_t lastDraw = 0;
  const int percent = job.size ? static_cast<int>(job.done * 100 / job.size) : 0;
  if (percent == shownPercent || millis() - lastDraw < 1000 || epd.isBusy()) return;
  shownPercent = percent;
  lastDraw = millis();
  shell::wake();
  drawJob();
  shell::refresh(false);
}

void startJob(const String &action, const String &name, const String &version, size_t size,
              const uint8_t *icon = nullptr) {
  job.stage = Stage::Running;
  job.action = action;
  job.name = name;
  job.version = version;
  job.size = size;
  job.done = 0;
  job.error = "";
  job.hasIcon = icon != nullptr;
  if (icon) memcpy(job.icon, icon, sizeof(job.icon));
  job.failureShown = false;
  job.rescue = false;
  hasCartridge = false;  // the old cartridge is being overwritten
  shell::wake();
  drawJob();
  shell::refresh(true);
}

void failJob(const String &error) {
  job.stage = Stage::Failed;
  job.error = error;
}

// ---------- BLE install ----------

// Flow control for the app: progress every kProgressStep bytes received, and resend
// requests (rate-limited) when writes were lost.
void notifyProgress() {
  static size_t lastReported = 0;
  static uint32_t lastResend = 0;
  size_t from;
  if (millis() - lastResend >= 300 && installer::takeResendRequest(from)) {
    lastResend = millis();
    JsonDocument event;
    event["event"] = "install.resend";
    event["from"] = from;
    ble::notify(event);
  }

  const size_t received = installer::received();
  if (received < lastReported) lastReported = 0;  // a new install started
  if (received == lastReported) return;
  if (received - lastReported < installer::kProgressStep && received != installer::payloadSize()) return;
  lastReported = received;
  JsonDocument event;
  event["event"] = "install.progress";
  event["received"] = received;
  event["written"] = installer::written();
  event["size"] = installer::meta().size;
  ble::notify(event);
}

bool parseSha256(const char *hex, uint8_t out[32]) {
  if (!hex || strlen(hex) != 64) return false;
  for (int i = 0; i < 32; i++) {
    char byte[3] = {hex[2 * i], hex[2 * i + 1], 0};
    char *end;
    out[i] = strtoul(byte, &end, 16);
    if (*end) return false;
  }
  return true;
}

String sha256Hex(const uint8_t *digest) {
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return hex;
}

void loopBleInstall() {
  // The app went away mid-install: give up (the slot holds a partial image now).
  static uint32_t disconnectedSince = 0;
  if (ble::connected()) {
    disconnectedSince = 0;
  } else if (disconnectedSince == 0) {
    disconnectedSince = millis();
  } else if (millis() - disconnectedSince > 5000) {
    disconnectedSince = 0;
    installer::abort();
    failJob("interrupted");
    return;
  }
  if (!job.hasIcon && installer::iconReady()) {
    memcpy(job.icon, installer::icon(), sizeof(job.icon));
    job.hasIcon = true;
  }
  job.done = installer::written();
  notifyProgress();
  updateJobScreen();
}

void registerBleInstallCommands() {
  installer::begin();
  ble::onData(installer::feed);

  ble::on("install.begin", [](JsonObjectConst args, JsonObject reply) {
    installer::Meta m;
    m.id = args["id"] | "";
    m.name = args["name"] | "";
    m.version = args["version"] | "";
    m.size = args["size"] | 0;
    m.hasIcon = args["icon"] | false;
    String error;
    if (m.id.isEmpty() || m.name.isEmpty()) error = "id and name are required";
    else if (m.id == "launcher") error = "the launcher updates with library.fetch (through Rescue)";
    else if (!parseSha256(args["sha256"], m.sha256)) error = "sha256 must be 64 hex characters";
    else installer::start(m, error);
    if (error.length()) {
      reply["ok"] = false;
      reply["error"] = error;
      return;
    }
    job.fromBle = true;
    startJob("Installing", m.name, m.version, m.size);
    reply["window"] = installer::kWindow;
    reply["iconBytes"] = installer::kIconBytes;
  });

  ble::on("install.end", [](JsonObjectConst, JsonObject reply) {
    String error;
    size_t missingFrom = 0;
    switch (installer::finish(error, missingFrom)) {
      case installer::Result::Done:
        job.stage = Stage::Done;  // loop() saves it to the card, then reboots into it
        break;
      case installer::Result::Missing:  // the last writes were lost: the app resends
        reply["ok"] = false;
        reply["missingFrom"] = missingFrom;
        break;
      case installer::Result::Failed:
        failJob(error);
        reply["ok"] = false;
        reply["error"] = error;
        break;
    }
  });

  ble::on("install.abort", [](JsonObjectConst, JsonObject) {
    installer::abort();
    failJob("cancelled");
  });
}

// Keeps a copy of a cartridge that just arrived over BLE, so switching back is fast.
void saveBleInstallToCard() {
  if (!library::available()) return;
  const installer::Meta &m = installer::meta();
  job.action = "Saving to card";
  job.stage = Stage::Running;
  drawJob();
  shell::refresh(false);
  library::Entry entry;
  entry.id = m.id;
  entry.name = m.name;
  entry.version = m.version;
  entry.size = m.size;
  entry.sha256 = sha256Hex(m.sha256);
  String error;
  if (!library::saveInstalled(entry, job.hasIcon ? job.icon : nullptr, error)) {
    LOGW("library", "not saved: %s", error.c_str());
  }
  job.stage = Stage::Done;
}

// ---------- launcher updates (through Rescue) ----------

// Launchers older than this predate flash layout 2 (they'd copy themselves over Rescue).
constexpr const char *kFirstRescueLauncher = "0.9.0";

bool olderThan(const String &a, const char *b) {
  int x[3] = {}, y[3] = {};
  sscanf(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]);
  sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
  for (int i = 0; i < 3; i++) {
    if (x[i] != y[i]) return x[i] < y[i];
  }
  return false;
}

// A new launcher on the card is installed by Rescue: it writes the launcher slot, checks it
// and starts it on trial (rescue/main.cpp). False (with error) if it can't be staged.
bool stageLauncher(const library::Entry &entry, String &error) {
  if (olderThan(entry.version, kFirstRescueLauncher)) {
    error = "launcher " + entry.version + " is too old for this Dotty";
    return false;
  }
  Preferences p;
  p.begin("rescue", false);
  p.putString("install", entry.version);
  p.end();
  job.rescue = true;
  job.action = "Restarting into Rescue";
  LOGI("update", "launcher %s staged for Rescue", entry.version.c_str());
  return true;
}

// Once this launcher has run fine: it's the one Rescue puts back if a later update fails,
// so remember its version and keep a copy of it on the card (a USB-flashed launcher has none).
// If it was an update on trial, it's now a success.
void rememberGoodLauncher() {
  Preferences p;
  p.begin("rescue", false);
  if (p.getString("good", "") != cartridge::self().version) p.putString("good", cartridge::self().version);
  if (p.getString("trying", "") == cartridge::self().version) {
    p.remove("trying");
    p.putString("result", "updated");
    p.putString("resultVer", cartridge::self().version);
    p.putString("reason", "");
  }
  p.end();
  loadUpdateResult();
  library::Entry onCard;
  if (!library::available() || library::find("launcher", cartridge::self().version, onCard)) return;
  String error;
  if (!library::saveRunning("launcher", "Launcher", cartridge::self().version, error)) {
    LOGW("update", "no copy of this launcher on the card: %s", error.c_str());
  }
}

// ---------- SD card library ----------

void registerLibraryCommands() {
  library::begin();

  ble::on("library.list", [](JsonObjectConst, JsonObject reply) {
    reply["card"] = library::available();
    JsonArray list = reply["cartridges"].to<JsonArray>();
    for (const library::Entry &e : library::list()) {
      JsonObject item = list.add<JsonObject>();
      item["id"] = e.id;
      item["name"] = e.name;
      item["version"] = e.version;
      item["size"] = e.size;
      item["sha256"] = e.sha256;
    }
  });

  // {id, version?, sha256?}: version defaults to the newest on the card; a sha256 makes
  // sure the card holds exactly the build the app expects.
  ble::on("install.fromCard", [](JsonObjectConst args, JsonObject reply) {
    library::Entry entry;
    const String id = args["id"] | "";
    const String version = args["version"] | "";
    const String sha = args["sha256"] | "";
    String error;
    if (!library::available()) error = "no SD card";
    else if (!library::find(id, version, entry)) error = "not on the card";
    else if (sha.length() && !sha.equalsIgnoreCase(entry.sha256)) error = "a different build is on the card";
    if (error.length()) {
      reply["ok"] = false;
      reply["error"] = error;
      return;
    }

    job.fromBle = false;
    startJob("Installing from card", entry.name, entry.version, entry.size);
    job.hasIcon = library::readIcon(entry, job.icon);
    if (entry.id == "launcher") {
      if (stageLauncher(entry, error)) {
        job.stage = Stage::Done;  // loop() restarts into Rescue
        reply["version"] = entry.version;
      } else {
        failJob(error);
        reply["ok"] = false;
        reply["error"] = error;
      }
      return;
    }
    const bool ok = library::install(entry, [](size_t done, size_t) {
      job.done = done;
      updateJobScreen();
    }, error);
    if (ok) {
      job.stage = Stage::Done;
      reply["version"] = entry.version;
    } else {
      failJob(error);
      reply["ok"] = false;
      reply["error"] = error;
    }
  });
}

// ---------- Wi-Fi: fetch cartridges straight from the GitHub catalog ----------

void notifyFetch(const char *stage, size_t done, size_t size) {
  static uint32_t last = 0;
  if (done != size && millis() - last < 500) return;
  last = millis();
  JsonDocument event;
  event["event"] = "fetch.progress";
  event["stage"] = stage;
  event["done"] = done;
  event["size"] = size;
  ble::notify(event);
}

// Downloads entry's firmware onto the card, verifying size and SHA-256.
bool downloadToCard(const library::Entry &entry, const String &url, String &error) {
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
      url,
      [&](const uint8_t *data, size_t len) {
        mbedtls_sha256_update(&sha, data, len);
        received += len;
        return out.write(data, len) == len;
      },
      [](size_t done, size_t) {
        job.done = done;
        updateJobScreen();
        notifyFetch("download", done, job.size);
      },
      error);
  out.close();
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (ok && received != entry.size) error = "download incomplete";
  else if (ok && sha256Hex(digest) != entry.sha256) error = "download corrupted (SHA-256)";
  else if (!ok && error.isEmpty()) error = "card write failed";
  if (error.length()) {
    SD_MMC.remove(path);
    return false;
  }
  return library::commit(entry, job.hasIcon ? job.icon : nullptr, error);
}

void registerFetchCommand() {
  // {id, version?, sha256?, install? = true}: Dotty reads the catalog over Wi-Fi,
  // downloads the cartridge onto the card (skipped if that build is already there),
  // then installs it from the card. Progress: fetch.progress events.
  ble::on("library.fetch", [](JsonObjectConst args, JsonObject reply) {
    const String id = args["id"] | "";
    const String wantVersion = args["version"] | "";
    const String wantSha = args["sha256"] | "";
    const bool install = args["install"] | true;
    const String catalogUrl = args["catalog"] | kCatalogUrl;
    String error;
    auto fail = [&](const String &message) {
      net::disconnect();
      failJob(message);
      reply["ok"] = false;
      reply["error"] = message;
    };
    if (id.isEmpty()) return fail("id is required");
    if (!library::available()) return fail("no SD card");

    // Show what we already know: the app sends name/version/size from its catalog copy,
    // and an earlier version on the card provides the icon.
    job.fromBle = false;
    library::Entry previous;
    uint8_t cardIcon[installer::kIconBytes];
    const bool haveIcon = library::find(id, "", previous) && library::readIcon(previous, cardIcon);
    startJob("Connecting to Wi-Fi", args["name"] | (previous.name.length() ? previous.name : id), wantVersion,
             args["size"] | 0, haveIcon ? cardIcon : nullptr);
    if (!net::connect(error)) return fail(error);

    notifyFetch("catalog", 0, 0);
    String body;
    if (!net::getString(catalogUrl, body, error)) return fail("catalog: " + error);
    JsonDocument catalog;
    if (deserializeJson(catalog, body) != DeserializationError::Ok) return fail("catalog is not valid JSON");
    JsonObjectConst item;
    for (JsonObjectConst e : catalog["cartridges"].as<JsonArrayConst>()) {
      if (id == (e["id"] | "")) item = e;
    }
    if (item.isNull()) return fail("not in the catalog");

    library::Entry entry;
    entry.id = id;
    entry.name = item["name"] | id;
    entry.version = item["version"] | "";
    entry.size = item["size"] | 0;
    entry.sha256 = item["sha256"] | "";
    if (wantVersion.length() && wantVersion != entry.version) return fail("catalog has " + entry.version);
    if (wantSha.length() && !wantSha.equalsIgnoreCase(entry.sha256)) return fail("catalog build differs");

    size_t iconLen = 0;
    const char *icon64 = item["icon"] | "";
    uint8_t catalogIcon[installer::kIconBytes];
    if (mbedtls_base64_decode(catalogIcon, sizeof(catalogIcon), &iconLen,
                              reinterpret_cast<const uint8_t *>(icon64), strlen(icon64)) == 0 &&
        iconLen == sizeof(catalogIcon)) {
      memcpy(job.icon, catalogIcon, sizeof(job.icon));
      job.hasIcon = true;
    } else {
      LOGW("fetch", "catalog icon missing or invalid (%u chars)", static_cast<unsigned>(strlen(icon64)));
    }
    job.name = entry.name;
    job.version = entry.version;
    job.size = entry.size;

    library::Entry onCard;
    const bool cached = library::find(id, entry.version, onCard) && onCard.sha256 == entry.sha256;
    job.action = cached ? "Already on the card" : "Downloading";
    drawJob();
    shell::refresh(false);
    if (!cached) {
      if (!downloadToCard(entry, item["firmware"] | "", error)) return fail(error);
    }
    net::disconnect();
    reply["version"] = entry.version;
    reply["downloaded"] = !cached;

    if (!install) {
      job.stage = Stage::None;
      hasCartridge = cartridge::readInstalled(installed);
      shell::showApp();
      return;
    }
    if (id == "launcher") {
      if (!stageLauncher(entry, error)) return fail(error);
      notifyFetch("install", entry.size, entry.size);
      job.stage = Stage::Done;  // loop() restarts into Rescue
      return;
    }
    job.action = "Installing from card";
    job.done = 0;
    if (!library::install(entry, [](size_t done, size_t) {
          job.done = done;
          updateJobScreen();
          notifyFetch("install", done, job.size);
        }, error)) {
      return fail(error);
    }
    job.stage = Stage::Done;  // loop() reboots into it
  });
}

// ---------- shell hooks ----------

// "Erase everything?" with Yes (left) / No (right).
void drawResetQuestion() {
  epd.fillScreen(kWhite);
  epd.fillRect(0, 0, kW, 45, kBlack);
  epd.setTextColor(kWhite);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, "Factory reset", 29);
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSansBold9pt7b);
  ui::drawCentered(epd, "Erase everything?", 72);
  epd.setFont(&FreeSans9pt7b);
  ui::drawWrapped(epd, "Songs, photos, recordings, Wi-Fi, pairings and settings.", 98, kW - 16, 3, 18);
  epd.fillRoundRect(12, 152, 84, 34, 17, kBlack);
  epd.setTextColor(kWhite);
  epd.setFont(&FreeSansBold9pt7b);
  epd.setCursor(12 + (84 - ui::textWidth(epd, "Erase")) / 2, 174);
  epd.print("Erase");
  epd.setTextColor(kBlack);
  epd.drawRoundRect(104, 152, 84, 34, 17, kBlack);
  epd.drawRoundRect(105, 153, 82, 32, 16, kBlack);
  epd.setCursor(104 + (84 - ui::textWidth(epd, "Keep")) / 2, 174);
  epd.print("Keep");
}

// Rescue does the erasing; this only leaves it the note (and the newer launcher to keep).
void startFactoryReset(const String &newerLauncher) {
  Preferences p;
  p.begin("rescue", false);
  p.putBool("reset", true);
  if (newerLauncher.length()) p.putString("install", newerLauncher);
  else p.remove("install");
  p.end();
  LOGW("launcher", "factory reset requested");
  cartridge::rebootToRescue();
}

// A whole-card backup or restore over Wi-Fi (tools/card_backup.py).
void drawCardTransfer() {
  const transfer::Status s = transfer::status();
  epd.fillScreen(kWhite);
  epd.fillRect(0, 0, kW, 45, kBlack);
  epd.setTextColor(kWhite);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, "SD card copy", 29);
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSansBold9pt7b);
  ui::drawCentered(epd, "Copying files", 80);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, ui::fitText(epd, ui::printable(s.file), kW - 16), 110);
  ui::drawCentered(epd, "Keep me close to Wi-Fi", 170);
}

void drawApp() {
  if (transfer::active()) {
    drawCardTransfer();
  } else if (askResetUntil) {
    drawResetQuestion();
  } else if (job.stage == Stage::None) {
    drawHome();
  } else {
    drawJob();
  }
}

const shell::Picture kOffPictures[] = {
    {kFirefly, kFireflyWidth, kFireflyHeight},
};

}  // namespace

void setup() {
#ifdef DOTTY_TEST_CRASH
  // Test build only (PLATFORMIO_BUILD_FLAGS=-DDOTTY_TEST_CRASH): a launcher that never starts,
  // to check the bootloader rollback and Rescue's automatic repair.
  delay(500);
  abort();
#endif
  shell::Config config;
  config.drawApp = drawApp;
  config.offPictures = kOffPictures;
  config.offPictureCount = 1;
  shell::begin(config);

  hasCartridge = cartridge::readInstalled(installed);
  if (hasCartridge && strcmp(installed.id, "launcher") == 0) {
    // What's left of a launcher update (see installSelfIfUpdate): not a cartridge.
    cartridge::eraseInstalled();
    hasCartridge = false;
    LOGI("update", "launcher update finished: %s", cartridge::self().version);
  }
  // Removing the installed cartridge (storage.remove) empties the slot as well.
  storage::onRemove([](const String &id) {
    if (!hasCartridge || id != installed.id) return;
    cartridge::eraseInstalled();
    hasCartridge = false;
    shell::showApp();
  });
  if (hasCartridge) {
    LOGI("launcher", "installed: %s %s", installed.name, installed.version);
  } else {
    LOGI("launcher", "no cartridge installed");
  }
  loadUpdateResult();
  ble::extendInfo([](JsonObject info) {
    info["card"] = library::available();
    info["wifi"] = !net::saved().empty();
    if (lastUpdate.status.length()) {
      JsonObject update = info["update"].to<JsonObject>();
      update["status"] = lastUpdate.status;
      update["version"] = lastUpdate.version;
      if (lastUpdate.reason.length()) update["reason"] = lastUpdate.reason;
    }
    if (!hasCartridge) {
      info["installed"] = nullptr;
      return;
    }
    JsonObject cart = info["installed"].to<JsonObject>();
    cart["id"] = installed.id;
    cart["name"] = installed.name;
    cart["version"] = installed.version;
  });
  // {launcher?}: erase everything (Rescue does it after this reply), keeping this launcher or
  // the newer one the app has just downloaded to the card (library.fetch, install: false).
  ble::on("launcher.factoryReset", [](JsonObjectConst args, JsonObject reply) {
    const String newer = args["launcher"] | "";
    library::Entry entry;
    if (newer.length() && !library::find("launcher", newer, entry)) {
      reply["ok"] = false;
      reply["error"] = "launcher " + newer + " isn't on the card";
      return;
    }
    resetNewer = newer;
    resetRequested = true;  // from loop(), once this reply has gone out
  });
  // The app has told the user how the last launcher update went.
  ble::on("launcher.updateSeen", [](JsonObjectConst, JsonObject) {
    Preferences p;
    p.begin("rescue", false);
    p.remove("result");
    p.remove("resultVer");
    p.remove("reason");
    p.end();
    loadUpdateResult();
    shell::showApp();
  });
  ble::on("launcher.start", [](JsonObjectConst, JsonObject reply) {
    if (!hasCartridge) {
      reply["ok"] = false;
      reply["error"] = "no cartridge installed";
      return;
    }
    startRequested = true;  // started from loop() once this reply has gone out
  });
  registerBleInstallCommands();
  transfer::registerCommands([](const transfer::Summary &) { shell::showApp(); });
  // wifi.add blocks while it joins: say so first (the refresh runs meanwhile).
  net::onJoining([](const String &ssid) {
    if (job.stage != Stage::None || askResetUntil) return;
    joiningSsid = ssid;
    drawHome();
    shell::refresh(false);
  });
  registerLibraryCommands();
  registerFetchCommand();
  shell::showApp();
}

void loop() {
  shell::Input input;
  const bool unlocked = shell::update(input);
  static bool remembered = false;
  if (!remembered && millis() > 8000) {  // after the shell's trial confirmation
    remembered = true;
    rememberGoodLauncher();
  }
  transfer::poll();
  if (joiningSsid.length()) {  // wifi.add has returned (it ran inside shell::update)
    const std::vector<String> known = net::saved();
    if (std::find(known.begin(), known.end(), joiningSsid) == known.end()) {
      joinFailedSsid = joiningSsid;
      joinFailedUntil = millis() + 6000;
    }
    joiningSsid = "";
    shell::showApp();
  }
  // The welcome steps follow along: pairing, Wi-Fi, the first cartridge.
  static Setup shownStage = setupStage();
  static bool failShown = false;
  const bool failNow = joinFailedUntil && millis() < joinFailedUntil;
  if ((setupStage() != shownStage || failShown != failNow) && job.stage == Stage::None && !askResetUntil &&
      !transfer::active() && !epd.isBusy()) {
    shownStage = setupStage();
    failShown = failNow;
    if (!failNow) joinFailedUntil = 0;
    shell::showApp();
  }
  if (!unlocked) return;
  if (transfer::active()) {  // a card copy: show what's moving, and stay awake
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw > 2000 && !epd.isBusy()) {
      lastDraw = millis();
      shell::wake();
      drawCardTransfer();
      shell::refresh(false);
    }
    delay(10);
    return;
  }

  // BOOT held 10 s on the home screen: ask about a factory reset. (BOOT held at power-on
  // can't be used: the chip would start in USB flashing mode.)
  static uint32_t bootDownAt = 0;
  const bool bootDown = digitalRead(PIN_BTN_BOOT) == LOW;
  if (!bootDown) bootDownAt = 0;
  else if (!bootDownAt) bootDownAt = millis();
  if (bootDown && bootDownAt && millis() - bootDownAt >= kResetHoldMs && job.stage == Stage::None && !askResetUntil) {
    askResetUntil = millis() + kResetAskMs;
    shell::showApp();
  }
  if (input.key == 'w') {  // developer aid: step through the welcome screens, then back to normal
    previewStage = previewStage >= static_cast<int>(Setup::Ready) ? -1 : previewStage + 1;
    shell::showApp();
  }
  if (input.key == 'f' && job.stage == Stage::None && !askResetUntil) {  // developer aid: the question
    askResetUntil = millis() + kResetAskMs;
    shell::showApp();
  }
  if (askResetUntil) input.boot = false;  // letting go of BOOT mustn't start the cartridge

  if (installer::active()) {
    loopBleInstall();
  } else if (job.stage == Stage::Done) {
    delay(300);  // let the reply to the last command go out
    if (job.fromBle) saveBleInstallToCard();
    drawJob();
    shell::refresh(false);
    epd.waitBusy();
    if (job.rescue) cartridge::rebootToRescue();
    cartridge::confirmHealthy();  // a deliberate restart, not a crash
    esp_restart();
  } else if (job.stage == Stage::Failed) {
    // Show the error until BOOT; the slot may no longer hold a valid cartridge.
    if (!job.failureShown && !epd.isBusy()) {
      job.failureShown = true;
      drawJob();
      shell::refresh(true);
    }
    if (input.boot) {
      job.stage = Stage::None;
      hasCartridge = cartridge::readInstalled(installed);
      shell::showApp();
    }
  } else if (resetRequested) {
    delay(300);  // let the reply go out
    startFactoryReset(resetNewer);
  } else if (askResetUntil) {
    // The question: Erase (left) / Keep (right); it also goes away by itself.
    if (input.gesture == Touch::Gesture::Tap && shell::touch.y() > 140) {
      if (shell::touch.x() < kW / 2) startFactoryReset("");
      askResetUntil = 0;
      shell::showApp();
    } else if (millis() > askResetUntil) {
      askResetUntil = 0;
      shell::showApp();
    }
  } else if ((input.boot || startRequested) && hasCartridge) {
    startCartridge();
  }
  delay(10);
}
