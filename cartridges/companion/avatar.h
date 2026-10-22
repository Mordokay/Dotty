#pragma once

#include <Adafruit_GFX.h>
#include <Arduino.h>

#include "screens.h"

// The companion's avatar. Until the art arrives (plan Task 7) a placeholder: a round face whose
// eyes, brows and mouth show the expression.
namespace avatar {

enum class Face { Idle, Blink, Listening, Thinking, TalkA, TalkB, Happy, Sad, Surprised, Laughing, Love, Sleeping };

Face forMood(const String &mood);
// The face for a moment of the conversation (state, the AI's mood, the animation clock).
Face faceFor(Talk state, const String &mood, int frame);
// Draws the avatar `size` px square (96 on the main screen, 48 small) with its top-left at x, y.
void draw(Adafruit_GFX &gfx, int16_t x, int16_t y, Face face, int frame, int16_t size);

}  // namespace avatar
