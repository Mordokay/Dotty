#pragma once

#include <Adafruit_GFX.h>
#include <Arduino.h>

#include "screens.h"

// The companion's avatar: ten characters (images/avatars.h, made by tools/avatar_art.py), each
// with twelve expressions, at 96 px (main screen) or 48 px (lock screen, text-only replies).
namespace avatar {

constexpr int16_t kBig = 96, kSmall = 48;  // the two sizes drawn (images/avatars.h)

// Same order as FACES in tools/avatar_art.py.
enum class Face { Idle, Blink, Listening, Thinking, TalkA, TalkB, Happy, Sad, Surprised, Laughing, Love, Sleeping };

Face forMood(const String &mood);
// The face for a moment of the conversation (state, the AI's mood, the animation clock).
Face faceFor(Talk state, const String &mood, int frame);
// Draws avatar `id` ("robot", "girl"…; unknown = the first) `size` px square (96 or 48) with its
// top-left at x, y. Idle faces bob 1 px on odd frames.
void draw(Adafruit_GFX &gfx, int16_t x, int16_t y, const String &id, Face face, int frame, int16_t size);
// The ids in the app's order, and their names.
int count();
const char *id(int i);
const char *name(int i);
bool known(const String &id);

}  // namespace avatar
