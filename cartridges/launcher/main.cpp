// Launcher: the permanent firmware in the factory partition. Shows which cartridge is
// installed and starts it, installs cartridges (over BLE, or from the SD card library),
// and keeps a copy of each installed cartridge on the card. Reached at boot when no
// valid cartridge is installed, or from a cartridge with BOOT + PWR / core.toLauncher.

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <SD_MMC.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include "art.h"
#include "battery.h"
#include "cartridge.h"
#include "core_ble.h"
#include "images/firefly.h"
#include "installer.h"
#include "library.h"
#include "log.h"
#include "net.h"
#include "storage.h"
#include "shell.h"
#include "ui.h"

DOTTY_CARTRIDGE("launcher", "Launcher", "0.6.7");

namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;
constexpr const char *kCatalogUrl = "https://github.com/Mordokay/Dotty/releases/latest/download/catalog.json";

CartridgeInfo installed;
bool hasCartridge = false;
bool startRequested = false;  // set by the launcher.start command

// What the progress screen shows: a BLE install, an install from the card, a download.
// Restarting / failing is acted on in loop(), after the command's reply has been sent.
enum class Stage { None, Running, Done, Failed };
struct Job {
  Stage stage = Stage::None;
  bool fromBle = false;  // progress comes from the BLE installer
  String action;         // "Installing", "Installing from card", …
  String name, version, error;
  size_t size = 0, done = 0;
  uint8_t icon[installer::kIconBytes];
  bool hasIcon = false;
  bool failureShown = false;
};
Job job;

void drawHome(const char *hint = nullptr) {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  epd.drawBitmap((kW - kFireflyWidth) / 2, 2, kFirefly, kFireflyWidth, kFireflyHeight, kBlack);
  ui::drawBattery(epd, kW - 8 - 29, 8, battery::percent(), battery::charging());

  epd.setFont(&FreeSansBold12pt7b);
  ui::drawCentered(epd, "Dotty", 144);

  epd.setFont(&FreeSans9pt7b);
  if (hasCartridge) {
    ui::drawCentered(epd, ui::fitText(epd, String(installed.name) + " " + installed.version, kW - 16),
                     168);
  } else {
    ui::drawCentered(epd, "No cartridge installed", 168);
  }
  if (hint) {
    ui::drawCentered(epd, hint, 192);
  } else if (hasCartridge) {
    ui::drawCentered(epd, "BOOT: start", 192);
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

void drawApp() {
  if (job.stage == Stage::None) {
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
  shell::Config config;
  config.drawApp = drawApp;
  config.offPictures = kOffPictures;
  config.offPictureCount = 1;
  shell::begin(config);

  hasCartridge = cartridge::readInstalled(installed);
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
  ble::extendInfo([](JsonObject info) {
    info["card"] = library::available();
    info["wifi"] = !net::saved().empty();
    if (!hasCartridge) {
      info["installed"] = nullptr;
      return;
    }
    JsonObject cart = info["installed"].to<JsonObject>();
    cart["id"] = installed.id;
    cart["name"] = installed.name;
    cart["version"] = installed.version;
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
  registerLibraryCommands();
  registerFetchCommand();
  shell::showApp();
}

void loop() {
  shell::Input input;
  if (!shell::update(input)) return;

  if (installer::active()) {
    loopBleInstall();
  } else if (job.stage == Stage::Done) {
    delay(300);  // let the reply to the last command go out
    if (job.fromBle) saveBleInstallToCard();
    drawJob();
    shell::refresh(false);
    epd.waitBusy();
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
  } else if ((input.boot || startRequested) && hasCartridge) {
    startCartridge();
  }
  delay(10);
}
