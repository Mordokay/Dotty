#include "art.h"

#include "epd_display.h"

namespace art {
namespace {

constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

}  // namespace

void drawPadlock(Adafruit_GFX &gfx, int16_t cx, int16_t top) {
  // Shackle: top half of a ring plus its two legs.
  const int16_t sy = top + 16;
  gfx.fillCircle(cx, sy, 15, kBlack);
  gfx.fillCircle(cx, sy, 9, kWhite);
  gfx.fillRect(cx - 16, sy, 32, 16, kWhite);
  gfx.fillRect(cx - 15, sy, 6, 12, kBlack);
  gfx.fillRect(cx + 9, sy, 6, 12, kBlack);

  // Body with a keyhole.
  gfx.fillRoundRect(cx - 22, sy + 10, 44, 34, 4, kBlack);
  gfx.fillCircle(cx, sy + 22, 5, kWhite);
  gfx.fillRect(cx - 2, sy + 24, 5, 11, kWhite);
}

void drawWifi(Adafruit_GFX &gfx, int16_t x, int16_t y, int16_t size) {
  // Designed on a 64 px grid: arcs radiate upwards from (32, 50), 45° either side.
  const float s = size / 64.0f;
  const float cx = 32 * s, cy = 50 * s;
  const float bands[3][2] = {{14 * s, 20 * s}, {26 * s, 32 * s}, {38 * s, 44 * s}};
  for (int16_t py = 0; py < size; py++) {
    for (int16_t px = 0; px < size; px++) {
      const float dx = px - cx, dy = cy - py;
      const float d = sqrtf(dx * dx + dy * dy);
      bool on = d <= 5.5f * s;  // the dot
      if (!on && dy > 0 && fabsf(dx) <= dy) {  // within 45° of straight up
        for (const auto &band : bands) on |= d >= band[0] && d < band[1];
      }
      if (on) gfx.drawPixel(x + px, y + py, kBlack);
    }
  }
}

namespace {

void fillBolt(Adafruit_GFX &gfx, int16_t cx, int16_t cy, int16_t height, uint16_t color) {
  // Two slanted wedges meeting in the middle: a classic zig-zag bolt.
  const int16_t h = height / 2, w = height / 3;
  gfx.fillTriangle(cx + w / 3, cy - h, cx - w, cy + h / 6, cx + w / 6, cy + h / 6, color);
  gfx.fillTriangle(cx - w / 3, cy + h, cx + w, cy - h / 6, cx - w / 6, cy - h / 6, color);
}

}  // namespace

void drawBolt(Adafruit_GFX &gfx, int16_t cx, int16_t cy, int16_t height, uint16_t color) {
  const uint16_t halo = color == kBlack ? kWhite : kBlack;
  for (int dx = -2; dx <= 2; dx++) {
    for (int dy = -2; dy <= 2; dy++) {
      if (dx || dy) fillBolt(gfx, cx + dx, cy + dy, height, halo);
    }
  }
  fillBolt(gfx, cx, cy, height, color);
}

void drawBigBattery(Adafruit_GFX &gfx, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t percent,
                    bool charging) {
  // Thick rounded outline, a nub on the right, and the level inside with a white gap.
  gfx.fillRoundRect(x, y, w, h, 6, kBlack);
  gfx.fillRoundRect(x + 4, y + 4, w - 8, h - 8, 3, kWhite);
  gfx.fillRect(x + w, y + h / 3, 6, h / 3, kBlack);
  const int16_t inner = w - 14;
  const int16_t level = inner * min<uint8_t>(percent, 100) / 100;
  if (level > 0) gfx.fillRect(x + 7, y + 7, level, h - 14, kBlack);
  if (charging) drawBolt(gfx, x + w / 2, y + h / 2, h - 10, kBlack);
}

}  // namespace art
