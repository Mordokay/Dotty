#include "weather_icons.h"

#include "epd_display.h"

namespace icons {
namespace {

constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

void thickLine(Adafruit_GFX &gfx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, int t) {
  for (int d = 0; d < t; d++) {
    gfx.drawLine(x0 + d, y0, x1 + d, y1, kBlack);
    gfx.drawLine(x0, y0 + d, x1, y1 + d, kBlack);
  }
}

void sun(Adafruit_GFX &gfx, int16_t cx, int16_t cy, int16_t r) {
  gfx.fillCircle(cx, cy, r * 45 / 100, kBlack);
  const int t = r >= 14 ? 3 : (r >= 8 ? 2 : 1);
  for (int i = 0; i < 8; i++) {
    const float a = i * PI / 4;
    thickLine(gfx, cx + cosf(a) * r * 0.62f, cy + sinf(a) * r * 0.62f, cx + cosf(a) * r * 0.95f,
              cy + sinf(a) * r * 0.95f, t);
  }
}

void moon(Adafruit_GFX &gfx, int16_t cx, int16_t cy, int16_t r) {
  gfx.fillCircle(cx, cy, r * 6 / 10, kBlack);
  gfx.fillCircle(cx + r * 3 / 10, cy - r * 2 / 10, r * 5 / 10, kWhite);
}

// A cloud in the box (x, y, w, h): a rounded base with two puffs on top. `grow` widens (or,
// negative, narrows) every part by that many pixels, so outlines keep an even width.
void cloudShape(Adafruit_GFX &gfx, int16_t x, int16_t y, int16_t w, int16_t h, int16_t grow, uint16_t color) {
  const int16_t baseH = h * 55 / 100, baseY = y + h - baseH;
  gfx.fillRoundRect(x - grow, baseY - grow, w + 2 * grow, baseH + 2 * grow, baseH / 2 + grow, color);
  gfx.fillCircle(x + w * 32 / 100, baseY + baseH / 6, h * 30 / 100 + grow, color);
  gfx.fillCircle(x + w * 60 / 100, y + h * 40 / 100, h * 40 / 100 + grow, color);
}

// `gap` clears a white margin around the cloud first (over a sun or moon behind it).
void cloud(Adafruit_GFX &gfx, int16_t x, int16_t y, int16_t w, int16_t h, bool outline, int16_t gap = 0) {
  if (gap) cloudShape(gfx, x, y, w, h, gap, kWhite);
  cloudShape(gfx, x, y, w, h, 0, kBlack);
  if (outline) cloudShape(gfx, x, y, w, h, -max<int16_t>(2, h / 10), kWhite);
}

void drops(Adafruit_GFX &gfx, int16_t x, int16_t y, int16_t w, int16_t h, int count, bool heavy) {
  const int t = h >= 10 ? 2 : 1;
  for (int i = 0; i < count; i++) {
    const int16_t dx = x + w * (i + 1) / (count + 1);
    thickLine(gfx, dx, y, dx - h / 3, y + h, heavy ? t + 1 : t);
  }
}

void dots(Adafruit_GFX &gfx, int16_t x, int16_t y, int16_t w, int16_t h) {
  const int16_t r = max<int16_t>(1, h / 6);
  for (int i = 0; i < 3; i++) {
    const int16_t dx = x + w * (i + 1) / 4;
    gfx.fillCircle(dx, y + (i % 2 ? h * 2 / 3 : h / 4), r, kBlack);
  }
}

void flakes(Adafruit_GFX &gfx, int16_t x, int16_t y, int16_t w, int16_t h) {
  const int16_t r = max<int16_t>(2, h / 3);
  for (int i = 0; i < 3; i++) {
    const int16_t fx = x + w * (i + 1) / 4, fy = y + (i % 2 ? h * 2 / 3 : h / 3);
    gfx.drawLine(fx - r, fy, fx + r, fy, kBlack);
    gfx.drawLine(fx, fy - r, fx, fy + r, kBlack);
    gfx.drawLine(fx - r * 7 / 10, fy - r * 7 / 10, fx + r * 7 / 10, fy + r * 7 / 10, kBlack);
    gfx.drawLine(fx - r * 7 / 10, fy + r * 7 / 10, fx + r * 7 / 10, fy - r * 7 / 10, kBlack);
  }
}

void bolt(Adafruit_GFX &gfx, int16_t cx, int16_t top, int16_t h) {
  const int16_t w = h * 6 / 10;
  gfx.fillTriangle(cx + w / 3, top, cx - w / 2, top + h * 6 / 10, cx + w / 8, top + h * 6 / 10, kBlack);
  gfx.fillTriangle(cx - w / 3, top + h, cx + w / 2, top + h * 4 / 10, cx - w / 8, top + h * 4 / 10, kBlack);
}

}  // namespace

void drawWeather(Adafruit_GFX &gfx, int code, bool isDay, int16_t cx, int16_t cy, int16_t size) {
  const int16_t x = cx - size / 2, y = cy - size / 2, s = size;
  const bool outline = size >= 32;
  // The cloud over precipitation: upper part of the box; what falls goes underneath.
  const int16_t cw = s * 90 / 100, ch = s * 58 / 100, cxl = x + s * 5 / 100, cy0 = y + s * 4 / 100;
  const int16_t fallY = cy0 + ch + s * 4 / 100, fallH = s - (fallY - y) - 1;

  if (code == 0) {
    isDay ? sun(gfx, cx, cy, s / 2) : moon(gfx, cx, cy, s / 2);
  } else if (code == 1 || code == 2) {
    if (isDay) {
      sun(gfx, x + s * 36 / 100, y + s * 34 / 100, s * 34 / 100);
    } else {
      moon(gfx, x + s * 36 / 100, y + s * 32 / 100, s * 44 / 100);
    }
    cloud(gfx, x + s * 18 / 100, y + s * 42 / 100, s * 82 / 100, s * 54 / 100, outline, max<int16_t>(2, s / 16));
  } else if (code == 3) {
    cloud(gfx, x, y + s * 18 / 100, s, s * 64 / 100, outline);
  } else if (code == 45 || code == 48) {  // fog
    const int t = s >= 32 ? 3 : 2;
    for (int i = 0; i < 4; i++) {
      const int16_t ly = y + s * (20 + i * 20) / 100, inset = (i % 2) * s / 8;
      gfx.fillRoundRect(x + inset, ly, s - 2 * inset, t, t / 2, kBlack);
    }
  } else if (code >= 51 && code <= 57) {  // drizzle
    cloud(gfx, cxl, cy0, cw, ch, outline);
    dots(gfx, cxl, fallY, cw, fallH);
  } else if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) {  // rain, showers
    cloud(gfx, cxl, cy0, cw, ch, outline);
    const bool heavy = code == 65 || code == 82 || code == 67;
    drops(gfx, cxl, fallY, cw, fallH, heavy ? 4 : 3, heavy);
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {  // snow
    cloud(gfx, cxl, cy0, cw, ch, outline);
    flakes(gfx, cxl, fallY, cw, fallH);
  } else if (code >= 95) {  // thunder
    cloud(gfx, cxl, cy0, cw, ch, outline);
    bolt(gfx, cx, fallY - s / 20, fallH + s / 20);
  } else {  // unknown: a plain cloud
    cloud(gfx, x, y + s * 18 / 100, s, s * 64 / 100, outline);
  }
}

}  // namespace icons
