#pragma once

#include <Adafruit_GFX.h>
#include <Arduino.h>

// The black bar across the top of cartridge screens: an icon button in each corner and a
// title in the middle. Corners are kButton wide; taps with y < kHeight hit the bar.
namespace nav {

constexpr int16_t kHeight = 30;
constexpr int16_t kButton = 44;

enum class Icon { None, Back, Forward, Shuffle, InOrder, Playlists, Star, StarFilled };

void draw(Adafruit_GFX &gfx, const String &title, Icon left, Icon right);

// One icon centred on (cx, cy) in `color` (white on the bar; black elsewhere, e.g. a grid).
void drawIcon(Adafruit_GFX &gfx, Icon icon, int16_t cx, int16_t cy, uint16_t color);

// Which corner a tap at (x, y) hit: -1 left, 1 right, 0 neither (title or below the bar).
int hit(uint16_t x, uint16_t y);

}  // namespace nav
