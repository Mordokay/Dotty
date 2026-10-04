#include "lock_screen.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include "epd_display.h"
#include "ui.h"

namespace {

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

// Closed eye: the lower half of a 2 px ring.
void drawClosedEye(Adafruit_GFX &gfx, int16_t cx, int16_t cy) {
  gfx.fillCircle(cx, cy, 8, kBlack);
  gfx.fillCircle(cx, cy, 6, kWhite);
  gfx.fillRect(cx - 9, cy - 9, 19, 9, kWhite);
}

void drawSleepingDotty(Adafruit_GFX &gfx, int16_t cx, int16_t cy) {
  drawClosedEye(gfx, cx - 18, cy);
  drawClosedEye(gfx, cx + 18, cy);
  gfx.drawCircle(cx, cy + 16, 3, kBlack);  // small "o" mouth, breathing
  gfx.setFont(&FreeSansBold9pt7b);
  gfx.setCursor(cx + 34, cy - 4);
  gfx.print("z");
  gfx.setCursor(cx + 44, cy - 16);
  gfx.print("Z");
}

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
  ui::drawBattery(gfx, kW - 8 - 29, 8, info.batteryPercent);
  gfx.setCursor(kW - 8 - 29 - 6 - pctW, 20);
  gfx.print(pct);

  // Big clock.
  char clock[6] = "--:--";
  if (info.timeValid) snprintf(clock, sizeof(clock), "%02d:%02d", info.time.tm_hour, info.time.tm_min);
  gfx.setFont(&FreeSansBold24pt7b);
  ui::drawCentered(gfx, clock, 82);

  drawSleepingDotty(gfx, kW / 2 - 8, 122);

  // Bottom line: what's playing, or how to unlock.
  gfx.drawFastHLine(20, 166, kW - 40, kBlack);
  gfx.setFont(&FreeSans9pt7b);
  if (info.nowPlaying.length() > 0) {
    const String title = ui::fitText(gfx, info.nowPlaying, kW - 40);
    const int16_t w = ui::textWidth(gfx, title) + 16;
    ui::drawNote(gfx, (kW - w) / 2, 177);
    gfx.setCursor((kW - w) / 2 + 16, 190);
    gfx.print(title);
  } else {
    ui::drawCentered(gfx, "press PWR to unlock", 190);
  }
}
