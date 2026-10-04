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

}  // namespace art
