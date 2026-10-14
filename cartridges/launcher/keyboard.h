#pragma once

#include <Arduino.h>

// The launcher's on-screen keyboard (Wi-Fi passwords, the touch calibration's typing test):
// 8 columns x 4 rows of 25 x 31 px keys at the bottom of the screen, like a phone's: letters
// first (QWERTY, each row's overflow moved down to the right end of the row below), "123" for
// digits and punctuation, "#+=" for the rest of ASCII. Shift is one-shot; tapped twice in a
// row it locks (caps lock). The screen above the keys (y < kTop) is the caller's.
namespace keyboard {

constexpr int16_t kKeyW = 25, kKeyH = 31;
constexpr int kCols = 8, kRows = 4;
constexpr int16_t kTop = 199 - kRows * kKeyH;  // 75: the last row's bottom edge on the screen's last line

enum class Result { None, Typed, Deleted, Changed };  // Changed: shift or the page

void reset();  // letters, no shift
void draw();
// A tap at (x, y) with y >= kTop: edits `text` (up to `max` characters).
Result tap(uint16_t x, uint16_t y, String &text, size_t max);
// Developer aid: a character from the serial monitor ('^' = shift, '#' = next page).
Result key(char c, String &text, size_t max);

}  // namespace keyboard
