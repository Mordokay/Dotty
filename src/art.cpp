#include "art.h"

#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include "epd_display.h"

namespace art {
namespace {

constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

void fillEllipse(Adafruit_GFX &gfx, int16_t cx, int16_t cy, int16_t rx, int16_t ry, uint16_t color) {
  for (int16_t dy = -ry; dy <= ry; dy++) {
    const float t = static_cast<float>(dy) / ry;
    const int16_t half = rx * sqrtf(1.0f - t * t);
    gfx.drawFastHLine(cx - half, cy + dy, 2 * half + 1, color);
  }
}

// White shape with a black outline of the given thickness.
void outlinedEllipse(Adafruit_GFX &gfx, int16_t cx, int16_t cy, int16_t rx, int16_t ry, int16_t stroke) {
  fillEllipse(gfx, cx, cy, rx, ry, kBlack);
  fillEllipse(gfx, cx, cy, rx - stroke, ry - stroke, kWhite);
}

// Thick stroke along a quadratic Bezier curve p0 -> p2, pulled towards p1.
void thickCurve(Adafruit_GFX &gfx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2,
                int16_t y2, int16_t radius, uint16_t color) {
  for (int i = 0; i <= 40; i++) {
    const float t = i / 40.0f, u = 1 - t;
    const int16_t x = u * u * x0 + 2 * u * t * x1 + t * t * x2;
    const int16_t y = u * u * y0 + 2 * u * t * y1 + t * t * y2;
    gfx.fillCircle(x, y, radius, color);
  }
}

// Closed eye: lower half of a ring, like a contented smile.
void closedEye(Adafruit_GFX &gfx, int16_t cx, int16_t cy) {
  gfx.fillCircle(cx, cy, 6, kBlack);
  gfx.fillCircle(cx, cy, 4, kWhite);
  gfx.fillRect(cx - 7, cy - 7, 15, 7, kWhite);
}

}  // namespace

void drawSleepingCat(Adafruit_GFX &gfx, int16_t cx, int16_t cy) {
  // Layout is designed around (100, 120); shift everything by the offset.
  const int16_t ox = cx - 100, oy = cy - 120;

  // Body: a loaf lying on its side.
  outlinedEllipse(gfx, ox + 118, oy + 132, 58, 30, 3);

  // Tail wrapping round the front of the body.
  thickCurve(gfx, ox + 172, oy + 146, ox + 160, oy + 190, ox + 100, oy + 170, 4, kBlack);

  // Ears (drawn before the head so the head overlaps their base).
  gfx.fillTriangle(ox + 44, oy + 112, ox + 46, oy + 80, ox + 70, oy + 100, kBlack);
  gfx.fillTriangle(ox + 70, oy + 98, ox + 92, oy + 82, ox + 92, oy + 112, kBlack);
  gfx.fillTriangle(ox + 50, oy + 104, ox + 51, oy + 89, ox + 62, oy + 99, kWhite);
  gfx.fillTriangle(ox + 78, oy + 97, ox + 87, oy + 90, ox + 87, oy + 104, kWhite);

  // Head resting on the body.
  outlinedEllipse(gfx, ox + 68, oy + 126, 30, 25, 3);

  closedEye(gfx, ox + 56, oy + 124);
  closedEye(gfx, ox + 80, oy + 124);
  gfx.fillTriangle(ox + 65, oy + 133, ox + 71, oy + 133, ox + 68, oy + 137, kBlack);  // nose

  // Whiskers.
  gfx.drawLine(ox + 46, oy + 134, ox + 26, oy + 130, kBlack);
  gfx.drawLine(ox + 46, oy + 138, ox + 26, oy + 140, kBlack);
  gfx.drawLine(ox + 90, oy + 134, ox + 110, oy + 130, kBlack);
  gfx.drawLine(ox + 90, oy + 138, ox + 110, oy + 140, kBlack);

  // Front paw peeking out under the chin.
  outlinedEllipse(gfx, ox + 96, oy + 152, 12, 7, 2);

  // Zzz, growing as they float up and away.
  gfx.setTextColor(kBlack);
  gfx.setFont(&FreeSansBold9pt7b);
  gfx.setCursor(ox + 108, oy + 92);
  gfx.print("z");
  gfx.setFont(&FreeSansBold12pt7b);
  gfx.setCursor(ox + 124, oy + 76);
  gfx.print("z");
  gfx.setFont(&FreeSansBold18pt7b);
  gfx.setCursor(ox + 144, oy + 56);
  gfx.print("Z");
}

}  // namespace art
