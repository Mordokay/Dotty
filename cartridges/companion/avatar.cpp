#include "avatar.h"

#include "epd_display.h"
#include "images/avatars.h"

namespace avatar {

Face forMood(const String &mood) {
  if (mood == "happy") return Face::Happy;
  if (mood == "sad") return Face::Sad;
  if (mood == "surprised") return Face::Surprised;
  if (mood == "thinking") return Face::Thinking;
  if (mood == "laughing") return Face::Laughing;
  if (mood == "love") return Face::Love;
  return Face::Idle;
}

Face faceFor(Talk state, const String &mood, int frame) {
  switch (state) {
    case Talk::Offline: return Face::Sleeping;
    case Talk::Connecting: return frame % 2 ? Face::Blink : Face::Sleeping;
    case Talk::Listening: return Face::Listening;
    case Talk::Thinking: return Face::Thinking;
    case Talk::Speaking: return frame % 2 ? Face::TalkB : Face::TalkA;
    case Talk::After: return forMood(mood);
    case Talk::Error: return Face::Sad;
    default: return frame % 4 == 3 ? Face::Blink : Face::Idle;  // Ready: a blink now and then
  }
}

namespace {

int indexOf(const String &id) {
  for (int i = 0; i < art::kAvatarCount; i++)
    if (id == art::kAvatarIds[i]) return i;
  return 0;
}

}  // namespace

void draw(Adafruit_GFX &g, int16_t x, int16_t y, const String &id, Face face, int frame, int16_t size) {
  const int a = indexOf(id), f = static_cast<int>(face);
  const int16_t bob = face == Face::Idle && frame % 2 ? 1 : 0;
  if (size >= art::kBigSize) g.drawBitmap(x, y + bob, art::kBig[a][f], art::kBigSize, art::kBigSize, EpdDisplay::kBlack);
  else g.drawBitmap(x, y + bob, art::kSmall[a][f], art::kSmallSize, art::kSmallSize, EpdDisplay::kBlack);
}

int count() {
  return art::kAvatarCount;
}
const char *id(int i) {
  return art::kAvatarIds[i];
}
const char *name(int i) {
  return art::kAvatarNames[i];
}
bool known(const String &id) {
  for (int i = 0; i < art::kAvatarCount; i++)
    if (id == art::kAvatarIds[i]) return true;
  return false;
}

}  // namespace avatar
