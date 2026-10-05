#include "lock_screen.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include "art.h"
#include "epd_display.h"
#include "ui.h"

namespace {

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

}  // namespace

void drawLockScreen(Adafruit_GFX &gfx, const LockScreenInfo &info) {
  static const char *kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  gfx.fillScreen(kWhite);
  gfx.setTextColor(kBlack);

  // Top row: date on the left, battery on the right.
  gfx.setFont(&FreeSans9pt7b);
  if (info.timeValid) {
    char date[16];
    snprintf(date, sizeof(date), "%s %d %s", kDays[info.time.tm_wday % 7], info.time.tm_mday,
             kMonths[info.time.tm_mon % 12]);
    gfx.setCursor(8, 20);
    gfx.print(date);
  }
  const String pct = String(info.batteryPercent) + "%";
  const int16_t pctW = ui::textWidth(gfx, pct);
  ui::drawBattery(gfx, kW - 8 - 29, 8, info.batteryPercent, info.charging);
  gfx.setCursor(kW - 8 - 29 - 6 - pctW, 20);
  gfx.print(pct);

  // Big clock.
  char clock[6] = "--:--";
  if (info.timeValid) snprintf(clock, sizeof(clock), "%02d:%02d", info.time.tm_hour, info.time.tm_min);
  gfx.setFont(&FreeSansBold24pt7b);
  ui::drawCentered(gfx, clock, 82);

  if (info.externalPower) {
    // Big battery filling up, with the exact percentage beside it.
    const int16_t bw = 84, bh = 40, gap = 12;
    gfx.setFont(&FreeSansBold18pt7b);
    const String big = String(info.batteryPercent) + "%";
    const int16_t textW = ui::textWidth(gfx, big);
    const int16_t x = (kW - (bw + 6 + gap + textW)) / 2;
    art::drawBigBattery(gfx, x, 106, bw, bh, info.batteryPercent, info.charging);
    gfx.setCursor(x + bw + 6 + gap, 139);
    gfx.print(big);
  } else {
    art::drawPadlock(gfx, kW / 2, 100);
  }

  // Bottom line: battery news first, then what's playing, or how to unlock.
  gfx.drawFastHLine(20, 166, kW - 40, kBlack);
  gfx.setFont(&FreeSans9pt7b);
  if (info.externalPower) {
    ui::drawCentered(gfx, info.charging ? "Charging" : "Fully charged", 190);
  } else if (info.batteryLow) {
    ui::drawCentered(gfx, "Battery low - charge soon", 190);
  } else if (info.nowPlaying.length() > 0) {
    const String title = ui::fitText(gfx, info.nowPlaying, kW - 40);
    const int16_t w = ui::textWidth(gfx, title) + 16;
    ui::drawNote(gfx, (kW - w) / 2, 177);
    gfx.setCursor((kW - w) / 2 + 16, 190);
    gfx.print(title);
  } else {
    ui::drawCentered(gfx, "press PWR to unlock", 190);
  }
}

void drawBatteryEmpty(Adafruit_GFX &gfx) {
  gfx.fillScreen(kWhite);
  gfx.setTextColor(kBlack);
  art::drawBigBattery(gfx, (kW - 106) / 2, 50, 100, 48, 2, false);
  gfx.setFont(&FreeSansBold12pt7b);
  ui::drawCentered(gfx, "Battery empty", 138);
  gfx.setFont(&FreeSans9pt7b);
  ui::drawCentered(gfx, "Charge Dotty to wake it", 172);
}

void drawPowerCard(Adafruit_GFX &gfx, uint8_t percent, bool external, bool charging) {
  gfx.fillScreen(kWhite);
  gfx.setTextColor(kBlack);
  art::drawBigBattery(gfx, (kW - 126) / 2, 34, 120, 56, percent, charging);
  gfx.setFont(&FreeSansBold18pt7b);
  ui::drawCentered(gfx, String(percent) + "%", 140);
  gfx.setFont(&FreeSans9pt7b);
  ui::drawCentered(gfx, !external ? "On battery" : charging ? "Charging" : "Fully charged", 176);
}
