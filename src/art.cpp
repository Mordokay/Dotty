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

}  // namespace art
