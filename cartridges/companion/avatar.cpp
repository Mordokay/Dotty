#include "avatar.h"

#include "avatar_pack.h"
#include "epd_display.h"
#include "images/avatars.h"

namespace avatar {

Face forMood(const String &mood) {
  if (mood == "happy") return Face::Happy;
  if (mood == "sad") return Face::Sad;
  if (mood == "surprised") return Face::Surprised;
  if (mood == "thinking") return Face::Thinking;
  if (mood == "laughing") return Face::Laughing;
  if (mood == "love" || mood == "tender") return Face::Love;
  // The wider range (16 moods) on the 12 faces: the closest one.
  if (mood == "excited" || mood == "proud") return Face::Happy;
  if (mood == "playful") return Face::Laughing;
  if (mood == "curious") return Face::Listening;
  if (mood == "worried" || mood == "embarrassed") return Face::Sad;
  if (mood == "annoyed") return Face::Thinking;
  if (mood == "sleepy") return Face::Sleeping;
  return Face::Idle;  // neutral, calm
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

AvatarPack pack(nullptr, 0);  // the others, from the card (setPack); the robot is built in

}  // namespace

const uint8_t *photoBig = nullptr, *photoSmall = nullptr;

// The mood badge on a photo: a white disc with a black ring on the lower right, and a tiny
// drawing of the expression.
void badge(Adafruit_GFX &g, int16_t cx, int16_t cy, int16_t r, Face face) {
  constexpr uint16_t K = EpdDisplay::kBlack, W = EpdDisplay::kWhite;
  g.fillCircle(cx, cy, r, W);
  g.drawCircle(cx, cy, r, K);
  g.drawCircle(cx, cy, r - 1, K);
  if (r < 10) {  // 48 px: only asleep or awake
    if (face == Face::Sleeping) g.drawChar(cx - 2, cy - 3, 'z', K, W, 1);
    else g.fillCircle(cx, cy, 2, K);
    return;
  }
  const int16_t e = r / 3;  // eye offset
  auto eyes = [&] { g.fillCircle(cx - e, cy - 3, 2, K), g.fillCircle(cx + e, cy - 3, 2, K); };
  switch (face) {
    case Face::Sleeping:
      g.drawLine(cx - 5, cy - 5, cx + 5, cy - 5, K), g.drawLine(cx + 5, cy - 5, cx - 5, cy + 5, K),
          g.drawLine(cx - 5, cy + 5, cx + 5, cy + 5, K);  // a Z
      break;
    case Face::TalkA:
    case Face::TalkB:  // sound waves
      g.fillCircle(cx - 5, cy, 2, K);
      for (int w = 0; w < 2; w++) g.drawCircleHelper(cx - 5, cy, 5 + 4 * w, 0x2 | 0x4, K);
      break;
    case Face::Love:
      g.fillCircle(cx - 3, cy - 2, 3, K), g.fillCircle(cx + 3, cy - 2, 3, K),
          g.fillTriangle(cx - 6, cy - 1, cx + 6, cy - 1, cx, cy + 6, K);
      break;
    case Face::Sad:
      eyes(), g.drawCircleHelper(cx, cy + 7, 5, 0x1 | 0x2, K);
      break;
    case Face::Surprised:
      eyes(), g.drawCircle(cx, cy + 4, 3, K);
      break;
    case Face::Thinking:
      g.drawChar(cx - 2, cy - 3, '?', K, W, 1);
      break;
    case Face::Laughing:
      eyes(), g.fillCircleHelper(cx, cy + 2, 5, 0x1, 0, K);
      break;
    default:  // happy, idle, listening, blink
      eyes(), g.drawCircleHelper(cx, cy + 1, 5, 0x4 | 0x8, K);
      break;
  }
}

void draw(Adafruit_GFX &g, int16_t x, int16_t y, const String &id, Face face, int frame, int16_t size) {
  if (id == "photo" && photoBig) {
    const bool big = size >= art::kBigSize;
    g.drawBitmap(x, y, big ? photoBig : photoSmall, big ? 128 : 48, big ? 128 : 48, EpdDisplay::kBlack);
    if (big) badge(g, x + 110, y + 110, 14, face);
    else badge(g, x + 41, y + 41, 7, face);
    return;
  }
  const int f = static_cast<int>(face);
  const bool big = size >= art::kBigSize;
  const int16_t bob = face == Face::Idle && frame % 2 ? 1 : 0;
  const uint8_t *bits = pack.face(id.c_str(), f, big);  // nullptr: not on the card yet (or the robot)
  if (!bits) bits = big ? art::kRobotBig[f] : art::kRobotSmall[f];
  const int16_t side = big ? art::kBigSize : art::kSmallSize;
  g.drawBitmap(x, y + bob, bits, side, side, EpdDisplay::kBlack);
}

void setPack(const uint8_t *data, size_t size) {
  pack = AvatarPack(data, size);
}

bool packed(const String &id) {
  return id == "robot" || id == "photo" || pack.face(id.c_str(), 0, true) != nullptr;
}

void setPhoto(const uint8_t *big, const uint8_t *small) {
  photoBig = big;
  photoSmall = small;
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
  if (id == "photo") return true;
  for (int i = 0; i < art::kAvatarCount; i++)
    if (id == art::kAvatarIds[i]) return true;
  return false;
}

}  // namespace avatar
