#pragma once

#include <Adafruit_GFX.h>

// Adafruit's font headers define their tables const at namespace scope, so every file that includes one gets
// its own copy (the big bold one, 8 KB, was in the Companion four times). Fonts shared by several files are
// included once, in fonts.cpp, and used through these.
extern const GFXfont &kBold24;  // FreeSansBold24pt7b
