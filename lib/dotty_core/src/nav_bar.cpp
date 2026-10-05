#include "nav_bar.h"

#include <Fonts/FreeSans9pt7b.h>

#include "epd_display.h"
#include "ui.h"

namespace nav {
namespace {

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

void thickLine(Adafruit_GFX &gfx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  for (int d = 0; d < 2; d++) {
    gfx.drawLine(x0, y0 + d, x1, y1 + d, color);
    gfx.drawLine(x0 + d, y0, x1 + d, y1, color);
  }
}

// An arrow head pointing right with its tip at (x, y).
void arrowHead(Adafruit_GFX &gfx, int16_t x, int16_t y, uint16_t color) {
  gfx.fillTriangle(x, y, x - 6, y - 5, x - 6, y + 5, color);
}

// Five-pointed star: triangles from the centre to each pair of neighbouring points.
void fillStar(Adafruit_GFX &gfx, int16_t cx, int16_t cy, float outer, float inner, uint16_t color) {
  int16_t px[10], py[10];
  for (int i = 0; i < 10; i++) {
    const float r = i % 2 ? inner : outer;
    const float a = -PI / 2 + i * PI / 5;
    px[i] = cx + lroundf(r * cosf(a));
    py[i] = cy + lroundf(r * sinf(a));
  }
  for (int i = 0; i < 10; i++) gfx.fillTriangle(cx, cy, px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], color);
}

}  // namespace

void drawIcon(Adafruit_GFX &gfx, Icon icon, int16_t cx, int16_t cy, uint16_t color) {
  const uint16_t back = color == kWhite ? kBlack : kWhite;
  switch (icon) {
    case Icon::Back:
      thickLine(gfx, cx + 5, cy - 10, cx - 5, cy, color);
      thickLine(gfx, cx - 5, cy, cx + 5, cy + 10, color);
      break;
    case Icon::Forward:
      thickLine(gfx, cx - 5, cy - 10, cx + 5, cy, color);
      thickLine(gfx, cx + 5, cy, cx - 5, cy + 10, color);
      break;
    case Icon::Shuffle:  // crossing arrows
      thickLine(gfx, cx - 11, cy - 7, cx + 6, cy + 6, color);
      thickLine(gfx, cx - 11, cy + 6, cx + 6, cy - 7, color);
      arrowHead(gfx, cx + 11, cy - 7, color);
      arrowHead(gfx, cx + 11, cy + 7, color);
      break;
    case Icon::InOrder:  // parallel arrows
      thickLine(gfx, cx - 11, cy - 6, cx + 6, cy - 6, color);
      thickLine(gfx, cx - 11, cy + 5, cx + 6, cy + 5, color);
      arrowHead(gfx, cx + 11, cy - 5, color);
      arrowHead(gfx, cx + 11, cy + 6, color);
      break;
    case Icon::Playlists:  // a list with a note
      for (int i = 0; i < 3; i++) gfx.fillRect(cx - 12, cy - 8 + i * 7, i == 2 ? 10 : 16, 3, color);
      gfx.fillCircle(cx + 7, cy + 7, 3, color);
      gfx.fillRect(cx + 9, cy - 6, 2, 13, color);
      gfx.fillRect(cx + 9, cy - 6, 5, 2, color);
      break;
    case Icon::Star:  // outline: a filled star with a smaller one cut out
      fillStar(gfx, cx, cy, 11, 4.6f, color);
      fillStar(gfx, cx, cy + 1, 7, 2.6f, back);
      break;
    case Icon::StarFilled:
      fillStar(gfx, cx, cy, 11, 4.6f, color);
      break;
    case Icon::Trash:  // lid with a handle, body narrowing down, two ribs
      gfx.fillRect(cx - 9, cy - 8, 19, 3, color);
      gfx.fillRect(cx - 3, cy - 11, 7, 3, color);
      thickLine(gfx, cx - 7, cy - 4, cx - 5, cy + 9, color);
      thickLine(gfx, cx + 7, cy - 4, cx + 5, cy + 9, color);
      gfx.fillRect(cx - 5, cy + 8, 11, 2, color);
      gfx.drawFastVLine(cx - 1, cy - 2, 9, color);
      gfx.drawFastVLine(cx + 2, cy - 2, 9, color);
      break;
    case Icon::None:
      break;
  }
}

void draw(Adafruit_GFX &gfx, const String &title, Icon left, Icon right, const String &rightText) {
  gfx.fillRect(0, 0, kW, kHeight, kBlack);
  gfx.setFont(&FreeSans9pt7b);
  gfx.setTextColor(kWhite);
  ui::drawCentered(gfx, ui::fitText(gfx, title, kW - 2 * kButton), kHeight / 2 + 7);
  if (rightText.length()) {
    gfx.setCursor(kW - 8 - ui::textWidth(gfx, rightText), kHeight / 2 + 7);
    gfx.print(rightText);
  }
  gfx.setTextColor(kBlack);
  drawIcon(gfx, left, kButton / 2, kHeight / 2, kWhite);
  if (rightText.isEmpty()) drawIcon(gfx, right, kW - kButton / 2, kHeight / 2, kWhite);
}

String pageLabel(int page, int pages) {
  return pages > 1 ? String(page + 1) + "/" + String(pages) : String();
}

int hit(uint16_t x, uint16_t y) {
  if (y >= kHeight) return 0;
  if (x < kTouch) return -1;
  if (x > kW - kTouch) return 1;
  return 0;
}

}  // namespace nav
