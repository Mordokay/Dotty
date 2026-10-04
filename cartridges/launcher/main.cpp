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

void drawApp() {
  drawHome();
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
  shell::showApp();
}

void loop() {
  shell::Input input;
  if (!shell::update(input)) return;

  if ((input.boot || startRequested) && hasCartridge) startCartridge();
  delay(10);
}
