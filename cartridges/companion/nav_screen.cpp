#include "nav_screen.h"

#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include "nav_bar.h"
#include "nav_lines.h"
#include "shell.h"
#include "ui.h"

namespace {

using shell::epd;
constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack, kWhite = EpdDisplay::kWhite;

// A line `w` pixels thick (arrows read from arm's length on a bike).
void thick(Adafruit_GFX &g, float x0, float y0, float x1, float y1, int w, uint16_t c) {
  const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
  if (len < 0.5f) return g.fillCircle(x0, y0, w / 2, c);
  const float nx = -dy / len * w / 2, ny = dx / len * w / 2;
  g.fillTriangle(x0 + nx, y0 + ny, x0 - nx, y0 - ny, x1 + nx, y1 + ny, c);
  g.fillTriangle(x0 - nx, y0 - ny, x1 - nx, y1 - ny, x1 + nx, y1 + ny, c);
  g.fillCircle(x0, y0, w / 2, c);
  g.fillCircle(x1, y1, w / 2, c);
}

// An arrowhead at (x, y) pointing along `deg` (0 = up, clockwise).
void head(Adafruit_GFX &g, float x, float y, float deg, float size, uint16_t c) {
  const float a = deg * PI / 180, back = size, half = size * 0.6f;
  const float ux = sinf(a), uy = -cosf(a);  // forward
  const float bx = x - ux * back, by = y - uy * back;
  g.fillTriangle(x + ux * size * 0.3f, y + uy * size * 0.3f, bx - uy * half, by + ux * half, bx + uy * half, by - ux * half, c);
}

void centeredIn(const String &text, int16_t y, int16_t x0, int16_t x1) {
  int16_t bx, by;
  uint16_t w, h;
  epd.getTextBounds(text, 0, y, &bx, &by, &w, &h);
  epd.setCursor(x0 + (x1 - x0 - static_cast<int16_t>(w)) / 2 - bx, y);
  epd.print(text);
}

// "120" + "m", or "1.2" + "km".
void splitDistance(int metres, String &number, String &unit) {
  const std::string words = distanceWords(metres);
  const size_t space = words.find(' ');
  number = words.substr(0, space).c_str();
  unit = space == std::string::npos ? "" : words.substr(space + 1).c_str();
}

}  // namespace

void drawArrow(Adafruit_GFX &g, Arrow a, int exit, int16_t cx, int16_t cy, int16_t size, uint16_t color) {
  const int w = size >= 80 ? 9 : 6;
  const float s = size;
  const float baseY = cy + s * 0.42f, joinY = cy + s * 0.02f;
  float turn = 0;
  switch (a) {
    case Arrow::Straight: turn = 0; break;
    case Arrow::SlightLeft: turn = -35; break;
    case Arrow::Left: turn = -90; break;
    case Arrow::SharpLeft: turn = -135; break;
    case Arrow::SlightRight: turn = 35; break;
    case Arrow::Right: turn = 90; break;
    case Arrow::SharpRight: turn = 135; break;
    case Arrow::UTurn: {  // up, round to the left, back down
      const float r = s * 0.18f, x = cx + r, top = cy - s * 0.15f;
      thick(g, x, baseY, x, top, w, color);
      for (int d = 0; d <= 180; d += 10) {
        const float a0 = d * PI / 180, a1 = (d + 10) * PI / 180;
        if (d < 180) thick(g, x - r + r * cosf(a0), top - r * sinf(a0), x - r + r * cosf(a1), top - r * sinf(a1), w, color);
      }
      thick(g, x - 2 * r, top, x - 2 * r, cy + s * 0.2f, w, color);
      head(g, x - 2 * r, cy + s * 0.32f, 180, s * 0.16f, color);
      return;
    }
    case Arrow::Roundabout: {  // a ring with the exit number, the way in from below, the exit arm
      const float r = s * 0.2f;
      for (int k = 0; k < w; k++) g.drawCircle(cx, cy, r - k / 2.0f, color);
      thick(g, cx, baseY, cx, cy + r, w, color);
      const float deg = exit == 1 ? 90 : exit == 2 ? 0 : exit == 3 ? -90 : -135;
      const float rad = deg * PI / 180, ex = cx + sinf(rad) * r, ey = cy - cosf(rad) * r;
      const float tx = cx + sinf(rad) * s * 0.42f, ty = cy - cosf(rad) * s * 0.42f;
      thick(g, ex, ey, tx, ty, w, color);
      head(g, tx + sinf(rad) * s * 0.08f, ty - cosf(rad) * s * 0.08f, deg, s * 0.16f, color);
      if (exit > 0) {
        g.setFont(size >= 80 ? &FreeSansBold12pt7b : &FreeSansBold9pt7b);
        g.setTextColor(color);
        int16_t bx, by;
        uint16_t tw, th;
        const String n(exit);
        g.getTextBounds(n, 0, 0, &bx, &by, &tw, &th);
        g.setCursor(cx - tw / 2 - bx, cy + th / 2);
        g.print(n);
      }
      return;
    }
    case Arrow::Arrive: {  // a flag on a pole
      const float px = cx - s * 0.18f;
      thick(g, px, baseY, px, cy - s * 0.42f, w, color);
      g.fillTriangle(px, cy - s * 0.42f, px + s * 0.45f, cy - s * 0.27f, px, cy - s * 0.12f, color);
      return;
    }
  }
  thick(g, cx, baseY, cx, joinY, w, color);  // the way you come in
  const float rad = turn * PI / 180, len = s * 0.34f;
  const float tx = cx + sinf(rad) * len, ty = joinY - cosf(rad) * len;
  thick(g, cx, joinY, tx, ty, w, color);
  head(g, tx + sinf(rad) * s * 0.1f, ty - cosf(rad) * s * 0.1f, turn, s * 0.18f, color);
}

void drawNav(const NavView &v) {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  if (v.confirm) {
    nav::draw(epd, "Stop?", nav::Icon::Back, nav::Icon::None);
    epd.setFont(&FreeSans12pt7b);
    ui::drawCentered(epd, "Stop the", 82);
    ui::drawCentered(epd, "directions?", 108);
    epd.fillRoundRect(12, 152, 80, 40, 6, kBlack);
    epd.drawRoundRect(108, 152, 80, 40, 6, kBlack);
    epd.setFont(&FreeSans9pt7b);
    epd.setTextColor(kWhite);
    centeredIn("Yes", 177, 12, 92);
    epd.setTextColor(kBlack);
    centeredIn("No", 177, 108, 188);
    return;
  }
  epd.setFont(&FreeSans9pt7b);
  nav::draw(epd, ui::fitText(epd, ui::printable(v.destination.c_str()), 96), nav::Icon::Close, nav::Icon::None,
            v.clock.c_str());
  drawArrow(epd, v.arrow, v.exit, 52, 94, 88, kBlack);
  String number, unit;
  splitDistance(v.toTurn, number, unit);
  epd.setFont(&FreeSansBold24pt7b);
  centeredIn(number, 108, 100, 198);
  epd.setFont(&FreeSans12pt7b);
  centeredIn(unit, 132, 100, 198);
  epd.setFont(&FreeSansBold9pt7b);
  if (v.waiting) {
    ui::drawCentered(epd, "Waiting for your iPhone...", 166);
  } else {
    const std::vector<String> street = ui::wrapText(epd, ui::printable(v.street.c_str()), kW - 16, 2);
    for (size_t i = 0; i < street.size(); i++) {
      epd.setCursor(8, 160 + i * 17);
      epd.print(street[i]);
    }
  }
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, String(distanceWords(v.left).c_str()) + " left - " + String(v.minutes) + " min", 196);
}

NavHit navHit(uint16_t x, uint16_t y, const NavView &v) {
  if (v.confirm) {
    if (nav::hit(x, y) < 0) return NavHit::No;
    if (y < 148) return NavHit::None;
    return x < kW / 2 ? NavHit::Yes : NavHit::No;
  }
  return nav::hit(x, y) < 0 ? NavHit::Stop : NavHit::None;
}

int16_t drawNavLockCard(Adafruit_GFX &c, const NavView &v) {
  c.fillScreen(kWhite);
  c.setTextColor(kBlack);
  drawArrow(c, v.arrow, v.exit, 34, 40, 64, kBlack);
  String number, unit;
  splitDistance(v.toTurn, number, unit);
  c.setFont(&FreeSansBold24pt7b);
  int16_t bx, by;
  uint16_t w, h;
  c.getTextBounds(number + " " + unit, 0, 0, &bx, &by, &w, &h);
  c.setCursor(72, 46);
  c.print(number);
  c.setFont(&FreeSans12pt7b);
  c.print(" ");
  c.print(unit);
  c.setFont(&FreeSans9pt7b);
  c.setCursor(72, 72);
  c.print(ui::fitText(c, ui::printable(v.street.c_str()), 124));
  return 80;
}

void drawNearby(const std::string &what, const std::vector<NearbyRow> &rows, int page, int pages) {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSans9pt7b);
  nav::draw(epd, ui::fitText(epd, String("Nearby: ") + ui::printable(what.c_str()), 110), nav::Icon::Back,
            nav::Icon::None, nav::pageLabel(page, pages));
  for (int i = 0; i < 4 && page * 4 + i < static_cast<int>(rows.size()); i++) {
    const NearbyRow &r = rows[page * 4 + i];
    const int16_t top = nav::kHeight + i * 38;
    epd.setFont(&FreeSansBold9pt7b);
    epd.setCursor(8, top + 15);
    epd.print(ui::fitText(epd, ui::printable(r.name.c_str()), kW - 16));
    epd.setFont(&FreeSans9pt7b);
    epd.setCursor(8, top + 31);
    epd.print(r.detail.c_str());
    if (i < 3 && page * 4 + i + 1 < static_cast<int>(rows.size())) epd.drawFastHLine(8, top + 37, kW - 16, kBlack);
  }
}

int nearbyHit(uint16_t x, uint16_t y, int rows) {
  if (y < nav::kHeight) {
    const int corner = nav::hit(x, y);
    return corner < 0 ? -2 : corner > 0 ? -3 : -1;
  }
  const int row = (y - nav::kHeight) / 38;
  return row < rows ? row : -1;
}
