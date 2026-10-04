#pragma once

#include <Adafruit_GFX.h>
#include <Arduino.h>

// Small drawing helpers shared by the screens. All coordinates assume a 200 px wide panel.
namespace ui {

int16_t textWidth(Adafruit_GFX &gfx, const String &text);
void drawCentered(Adafruit_GFX &gfx, const String &text, int16_t y);

// Shortens text with "..." until it fits maxWidth with the current font.
String fitText(Adafruit_GFX &gfx, String text, int16_t maxWidth);

// Black title bar across the top.
void drawHeader(Adafruit_GFX &gfx, const char *label);

// Battery outline with fill level, w x h at (x, y).
void drawBattery(Adafruit_GFX &gfx, int16_t x, int16_t y, uint8_t percent);

// Small eighth-note glyph, ~10 x 14 px with its top-left at (x, y).
void drawNote(Adafruit_GFX &gfx, int16_t x, int16_t y);

String formatDuration(uint32_t ms);

}  // namespace ui
