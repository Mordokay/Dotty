#pragma once

#include <Adafruit_GFX.h>
#include <Arduino.h>

// The black bar across the top of cartridge screens: an icon button in each corner and a
// title in the middle. Icons sit kButton from the edges; a tap with y < kHeight hits a
// corner when it lands in the outer kTouch (a third of the width each side: the title
// isn't a button, so the corners can be generous — the user missed the 30 px bar's arrows).
namespace nav {

constexpr int16_t kHeight = 45;
constexpr int16_t kButton = 44;
constexpr int16_t kTouch = 66;

enum class Icon { None, Back, Forward, Shuffle, InOrder, Playlists, Star, StarFilled, Trash };

// `rightText` (e.g. a list's page, "1/2") takes the right corner instead of an icon.
void draw(Adafruit_GFX &gfx, const String &title, Icon left, Icon right, const String &rightText = String());

// "2/3" for a paged list, "" when everything fits on one page.
String pageLabel(int page, int pages);

// One icon centred on (cx, cy) in `color` (white on the bar; black elsewhere, e.g. a grid).
void drawIcon(Adafruit_GFX &gfx, Icon icon, int16_t cx, int16_t cy, uint16_t color);

// Which corner a tap at (x, y) hit: -1 left, 1 right, 0 neither (title or below the bar).
int hit(uint16_t x, uint16_t y);

}  // namespace nav
