#pragma once

#include <Adafruit_GFX.h>

// Vector icons drawn from primitives: crisp at any size, no image files.
namespace art {

// Solid padlock, 44 x 60 px, horizontally centred on cx with its top at y.
void drawPadlock(Adafruit_GFX &gfx, int16_t cx, int16_t top);

// Wi-Fi symbol (three arcs over a dot) filling a size x size box at (x, y).
void drawWifi(Adafruit_GFX &gfx, int16_t x, int16_t y, int16_t size = 64);

}  // namespace art
