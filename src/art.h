#pragma once

#include <Adafruit_GFX.h>

// Vector icons drawn from primitives: crisp at any size, no image files.
namespace art {

// Solid padlock, 44 x 60 px, horizontally centred on cx with its top at y.
void drawPadlock(Adafruit_GFX &gfx, int16_t cx, int16_t top);

}  // namespace art
