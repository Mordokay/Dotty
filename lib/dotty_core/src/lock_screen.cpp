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

  char clock[6] = "--:--";
  if (info.timeValid) snprintf(clock, sizeof(clock), "%02d:%02d", info.time.tm_hour, info.time.tm_min);
  gfx.setFont(&FreeSansBold24pt7b);

  // With a widget (a joke, the forecast…): it draws into its own canvas and says how tall it
  // is; the clock row (clock + small padlock) and the widget are then centred together
  // between the top row and the bottom line, so a 3-line joke doesn't leave a gap.
  constexpr int16_t kWidgetMax = 80, kAreaTop = 26, kAreaBottom = 162, kGap = 12;
  int16_t widgetH = 0;
  static GFXcanvas1 area(kW, kWidgetMax);
  if (info.widget && info.timeValid) {
    area.fillScreen(kWhite);
    area.setTextColor(kBlack);
    area.setTextWrap(false);
    area.setFont(&FreeSans9pt7b);
    widgetH = constrain(info.widget(area, info.time, kWidgetMax), 0, kWidgetMax);
  }
  const bool widgetShown = widgetH > 0;
  if (widgetShown) {
    gfx.setFont(&FreeSansBold24pt7b);  // the widget may have changed it (it can share this display)
    int16_t x1, y1;
    uint16_t w, h;
    gfx.getTextBounds(clock, 0, 0, &x1, &y1, &w, &h);  // y1 < 0: the digits' top above the baseline
    const int16_t lockW = art::padlockWidth(h), gap = 12;  // padlock as tall as the digits
    const int16_t top = kAreaTop + (kAreaBottom - kAreaTop - (h + kGap + widgetH)) / 2;
    const int16_t x = (kW - (w + gap + lockW)) / 2;
    gfx.setCursor(x - x1, top - y1);
    gfx.print(clock);
    art::drawSmallPadlock(gfx, x + w + gap, top, h);
    // Canvas bits: 1 = white, like the display's.
    gfx.drawBitmap(0, top + h + kGap, area.getBuffer(), kW, widgetH, kWhite, kBlack);
  }

  if (widgetShown) {
    // The middle is the widget's; power and music news stay on the bottom line.
  } else if (info.externalPower) {
    ui::drawCentered(gfx, clock, 82);
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
    ui::drawCentered(gfx, clock, 82);
    art::drawPadlock(gfx, kW / 2, 100);
  }

  // Bottom line: what's playing (or a paused recording) first — on a charger the big
  // battery already says so — then battery news, or how to unlock.
  gfx.drawFastHLine(20, 166, kW - 40, kBlack);
  gfx.setFont(&FreeSans9pt7b);
  const bool chargerShown = info.externalPower && !widgetShown;
  if (info.externalPower && (info.nowPlaying.isEmpty() || !chargerShown)) {
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
