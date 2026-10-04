#include "ui.h"

#include <Fonts/FreeSans9pt7b.h>

#include "epd_display.h"

namespace ui {
namespace {

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

}  // namespace

int16_t textWidth(Adafruit_GFX &gfx, const String &text) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);
  return w;
}

void drawCentered(Adafruit_GFX &gfx, const String &text, int16_t y) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx.getTextBounds(text.c_str(), 0, y, &x1, &y1, &w, &h);
  gfx.setCursor((kW - w) / 2 - x1, y);
  gfx.print(text);
}

String fitText(Adafruit_GFX &gfx, String text, int16_t maxWidth) {
  if (textWidth(gfx, text) <= maxWidth) return text;
  while (text.length() > 1 && textWidth(gfx, text + "...") > maxWidth) {
    text.remove(text.length() - 1);
  }
  return text + "...";
}

void drawHeader(Adafruit_GFX &gfx, const char *label) {
  gfx.fillRect(0, 0, kW, 26, kBlack);
  gfx.setFont(&FreeSans9pt7b);
  gfx.setTextColor(kWhite);
  drawCentered(gfx, label, 18);
  gfx.setTextColor(kBlack);
}

void drawBattery(Adafruit_GFX &gfx, int16_t x, int16_t y, uint8_t percent) {
  const int16_t w = 26, h = 13;
  gfx.drawRect(x, y, w, h, kBlack);
  gfx.drawRect(x + 1, y + 1, w - 2, h - 2, kBlack);
  gfx.fillRect(x + w, y + 4, 3, h - 8, kBlack);  // terminal nub
  const int16_t fill = (w - 6) * min<uint8_t>(percent, 100) / 100;
  gfx.fillRect(x + 3, y + 3, fill, h - 6, kBlack);
}

void drawNote(Adafruit_GFX &gfx, int16_t x, int16_t y) {
  gfx.fillCircle(x + 3, y + 11, 3, kBlack);
  gfx.fillRect(x + 5, y, 2, 12, kBlack);
  gfx.fillTriangle(x + 7, y, x + 7, y + 6, x + 11, y + 5, kBlack);
}

String formatDuration(uint32_t ms) {
  char buf[12];
  const uint32_t s = ms / 1000;
  snprintf(buf, sizeof(buf), "%lu:%02lu", s / 60, s % 60);
  return buf;
}

}  // namespace ui
