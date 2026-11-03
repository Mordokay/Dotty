#pragma once

#include <Adafruit_GFX.h>
#include <Arduino.h>

#include "screens.h"

// The companion's avatar: ten characters (tools/avatar_art.py: the robot built in, images/avatars.h; the
// other nine from the card, setPack), each with twelve expressions, at 128 px (main screen) or 48 px
// (lock screen, text-only replies).
namespace avatar {

constexpr int16_t kBig = 128, kSmall = 48;  // the two sizes drawn (images/avatars.h)

// Same order as FACES in tools/avatar_art.py.
enum class Face { Idle, Blink, Listening, Thinking, TalkA, TalkB, Happy, Sad, Surprised, Laughing, Love, Sleeping };

Face forMood(const String &mood);
// The face for a moment of the conversation (state, the AI's mood, the animation clock).
Face faceFor(Talk state, const String &mood, int frame);
// Draws avatar `id` ("robot", "girl"…; unknown = the first) `size` px square (96 or 48) with its
// top-left at x, y. Idle faces bob 1 px on odd frames.
void draw(Adafruit_GFX &gfx, int16_t x, int16_t y, const String &id, Face face, int frame, int16_t size);
// A photo avatar (id "photo"): the 128 and 48 px circles from the companion's photo.bin
// (1-bit, MSB-first rows, set bit = black), or nullptr for none (then the robot is drawn). A photo
// can't change its face: a small mood badge on the circle's edge shows the expression instead.
constexpr size_t kPhotoBig = 128 * 128 / 8, kPhotoSmall = 48 * 48 / 8;
void setPhoto(const uint8_t *big, const uint8_t *small);
// The avatars on the card (assets::begin / a download: DAV1, avatar_pack.h); nullptr clears. The caller
// owns the buffer and keeps it alive. An avatar that isn't in it draws as the robot (built in).
void setPack(const uint8_t *data, size_t size);
bool packed(const String &id);  // robot, photo, or in the current pack: drawn as itself
// The ids in the app's order, and their names.
int count();
const char *id(int i);
const char *name(int i);
bool known(const String &id);

}  // namespace avatar
