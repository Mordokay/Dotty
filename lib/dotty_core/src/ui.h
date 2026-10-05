#pragma once

#include <Adafruit_GFX.h>
#include <Arduino.h>

#include <vector>

// Small drawing helpers shared by the screens. All coordinates assume a 200 px wide panel.
namespace ui {

// The fonts only have printable ASCII. Turns any UTF-8 text into what they can draw:
// curly quotes and dashes become plain ones, other characters (Korean, Chinese…) are
// dropped, and the empty brackets and double spaces they leave behind are cleaned up.
// "AKMU - ‘후라이의 꿈’ (Live)" → "AKMU - '' (Live)" → "AKMU - (Live)". May return "".
String printable(const String &text);

// Text helpers below call printable() themselves and never wrap on their own.
int16_t textWidth(Adafruit_GFX &gfx, const String &text);
void drawCentered(Adafruit_GFX &gfx, const String &text, int16_t y);

// Shortens text with "..." until it fits maxWidth with the current font.
String fitText(Adafruit_GFX &gfx, String text, int16_t maxWidth);

// Word-wraps text into at most maxLines lines of maxWidth; the last one ends in "..." if
// the text doesn't fit.
std::vector<String> wrapText(Adafruit_GFX &gfx, const String &text, int16_t maxWidth, int maxLines);

// Draws wrapText() lines centred, the first baseline at y, lineHeight apart. Returns the
// number of lines drawn.
int drawWrapped(Adafruit_GFX &gfx, const String &text, int16_t y, int16_t maxWidth, int maxLines,
                int16_t lineHeight);

// Black title bar across the top.
void drawHeader(Adafruit_GFX &gfx, const char *label);

// Battery outline with fill level, 26 x 13 (plus nub) at (x, y), with a bolt over it while
// charging.
void drawBattery(Adafruit_GFX &gfx, int16_t x, int16_t y, uint8_t percent, bool charging = false);

// Small eighth-note glyph, ~10 x 14 px with its top-left at (x, y).
void drawNote(Adafruit_GFX &gfx, int16_t x, int16_t y);

String formatDuration(uint32_t ms);

}  // namespace ui
