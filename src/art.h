#pragma once

#include <Adafruit_GFX.h>

// Vector illustrations drawn from primitives: crisp at any size, no image files.
namespace art {

// A cat curled up asleep with "Zzz", about 150 x 100 px, centred on (cx, cy).
void drawSleepingCat(Adafruit_GFX &gfx, int16_t cx, int16_t cy);

}  // namespace art
