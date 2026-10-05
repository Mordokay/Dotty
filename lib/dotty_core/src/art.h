#pragma once

#include <Adafruit_GFX.h>

// Vector icons drawn from primitives: crisp at any size, no image files.
namespace art {

// Solid padlock, 44 x 60 px, horizontally centred on cx with its top at y.
void drawPadlock(Adafruit_GFX &gfx, int16_t cx, int16_t top);

// Wi-Fi symbol (three arcs over a dot) filling a size x size box at (x, y).
void drawWifi(Adafruit_GFX &gfx, int16_t x, int16_t y, int16_t size = 64);

// Lightning bolt centred on (cx, cy), `height` tall, in `color` with a 1 px halo in the
// other colour so it shows on both a filled and an empty battery.
void drawBolt(Adafruit_GFX &gfx, int16_t cx, int16_t cy, int16_t height, uint16_t color);

// Big battery outline (w x h plus its nub) at (x, y), filled to percent, with a bolt
// while charging.
void drawBigBattery(Adafruit_GFX &gfx, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t percent,
                    bool charging);

}  // namespace art
