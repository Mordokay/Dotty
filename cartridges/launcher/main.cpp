// Launcher: the permanent firmware in the factory partition. Shows which cartridge is
// installed and starts it; installing cartridges over BLE comes next. Reached at boot
// when no valid cartridge is installed, or from a cartridge with BOOT + PWR.

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

#include "battery.h"
#include "cartridge.h"
#include "core_ble.h"
#include "images/firefly.h"
#include "installer.h"
#include "log.h"
#include "shell.h"
#include "ui.h"

DOTTY_CARTRIDGE("launcher", "Launcher", "0.1.0");

namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

CartridgeInfo installed;
bool hasCartridge = false;
bool startRequested = false;  // set by the launcher.start command

// Install state shown on screen. Restarting / failing is acted on in loop(), after the
// command's reply has been sent.
enum class InstallStage { None, Running, Done, Failed };
InstallStage installStage = InstallStage::None;
String installError;
bool failureShown = false;

void drawHome(const char *hint = nullptr) {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  epd.drawBitmap((kW - kFireflyWidth) / 2, 2, kFirefly, kFireflyWidth, kFireflyHeight, kBlack);
  ui::drawBattery(epd, kW - 8 - 29, 8, batteryPercent(batteryMillivolts()));

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

// ---------- install screen ----------

void drawInstall() {
  const installer::Meta &m = installer::meta();
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);

  const int16_t iconX = (kW - installer::kIconSize) / 2, iconY = 8;
  if (installer::iconReady()) {
    epd.drawBitmap(iconX, iconY, installer::icon(), installer::kIconSize, installer::kIconSize, kBlack);
  } else {
    epd.drawRoundRect(iconX, iconY, installer::kIconSize, installer::kIconSize, 8, kBlack);
  }

  epd.setFont(&FreeSans9pt7b);
  const char *stage = installStage == InstallStage::Done     ? "Starting"
                      : installStage == InstallStage::Failed ? "Install failed:"
                                                             : "Installing";
  ui::drawCentered(epd, stage, 96);
  epd.setFont(&FreeSansBold12pt7b);
  ui::drawCentered(epd, ui::fitText(epd, installStage == InstallStage::Failed ? installError : m.name,
                                    kW - 16),
                   122);

  const size_t written = installStage == InstallStage::Done ? m.size : installer::written();
  const int percent = m.size ? static_cast<int>(written * 100 / m.size) : 0;
  const int16_t barX = 20, barY = 138, barW = kW - 40, barH = 12;
  epd.drawRoundRect(barX, barY, barW, barH, 6, kBlack);
  epd.fillRoundRect(barX + 2, barY + 2, (barW - 4) * percent / 100, barH - 4, 4, kBlack);

  epd.setFont(&FreeSans9pt7b);
  char line[40];
  snprintf(line, sizeof(line), "%d%%  %u / %u KB", percent, static_cast<unsigned>(written / 1024),
           static_cast<unsigned>(m.size / 1024));
  ui::drawCentered(epd, line, 172);
  ui::drawCentered(epd, "v" + m.version, 194);
}

// Partial refresh about once a second while the percentage moves.
void updateInstallScreen() {
  static int shownPercent = -1;
  static uint32_t lastDraw = 0;
  const installer::Meta &m = installer::meta();
  const int percent = m.size ? static_cast<int>(installer::written() * 100 / m.size) : 0;
  if (percent == shownPercent || millis() - lastDraw < 1000 || epd.isBusy()) return;
  shownPercent = percent;
  lastDraw = millis();
  shell::wake();
  drawInstall();
  shell::refresh(false);
}

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

void registerInstallCommands() {
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
    // The old cartridge is being overwritten.
    hasCartridge = false;
    installStage = InstallStage::Running;
    failureShown = false;
    shell::wake();
    drawInstall();
    shell::refresh(true);
    reply["window"] = installer::kWindow;
    reply["iconBytes"] = installer::kIconBytes;
  });

  ble::on("install.end", [](JsonObjectConst, JsonObject reply) {
    String error;
    size_t missingFrom = 0;
    switch (installer::finish(error, missingFrom)) {
      case installer::Result::Done:
        installStage = InstallStage::Done;  // loop() shows "Starting" and reboots into it
        break;
      case installer::Result::Missing:  // the last writes were lost: the app resends
        reply["ok"] = false;
        reply["missingFrom"] = missingFrom;
        break;
      case installer::Result::Failed:
        installStage = InstallStage::Failed;
        installError = error;
        reply["ok"] = false;
        reply["error"] = error;
        break;
    }
  });

  ble::on("install.abort", [](JsonObjectConst, JsonObject) {
    installer::abort();
    installStage = InstallStage::Failed;
    installError = "cancelled";
  });
}

void drawApp() {
  if (installStage == InstallStage::None) {
    drawHome();
  } else {
    drawInstall();
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
  if (hasCartridge) {
    LOGI("launcher", "installed: %s %s", installed.name, installed.version);
  } else {
    LOGI("launcher", "no cartridge installed");
  }
  ble::extendInfo([](JsonObject info) {
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
  registerInstallCommands();
  shell::showApp();
}

void loop() {
  shell::Input input;
  if (!shell::update(input)) return;

  if (installer::active()) {
    // The app went away mid-install: give up (the slot holds a partial image now).
    static uint32_t disconnectedSince = 0;
    if (ble::connected()) {
      disconnectedSince = 0;
    } else if (disconnectedSince == 0) {
      disconnectedSince = millis();
    } else if (millis() - disconnectedSince > 5000) {
      disconnectedSince = 0;
      installer::abort();
      installStage = InstallStage::Failed;
      installError = "interrupted";
    }
    notifyProgress();
    updateInstallScreen();
  } else if (installStage == InstallStage::Done) {
    drawInstall();
    shell::refresh(false);
    epd.waitBusy();
    delay(300);  // let the install.end reply go out
    esp_restart();
  } else if (installStage == InstallStage::Failed) {
    // Show the error until BOOT; the slot no longer holds a valid cartridge.
    if (!failureShown && !epd.isBusy()) {
      failureShown = true;
      drawInstall();
      shell::refresh(true);
    }
    if (input.boot) {
      installStage = InstallStage::None;
      hasCartridge = cartridge::readInstalled(installed);
      shell::showApp();
    }
  } else if ((input.boot || startRequested) && hasCartridge) {
    startCartridge();
  }
  delay(10);
}
