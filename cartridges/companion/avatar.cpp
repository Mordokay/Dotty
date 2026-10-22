#include "avatar.h"

#include "epd_display.h"

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

void draw(Adafruit_GFX &g, int16_t x, int16_t y, Face face, int frame, int16_t size) {
  constexpr uint16_t kBlack = EpdDisplay::kBlack, kWhite = EpdDisplay::kWhite;
  const int16_t r = size / 2 - 2, cx = x + size / 2, cy = y + size / 2 + ((frame % 2) && face == Face::Idle ? 1 : 0);
  g.fillCircle(cx, cy, r, kWhite);
  g.drawCircle(cx, cy, r, kBlack);
  g.drawCircle(cx, cy, r - 1, kBlack);
  const int16_t ex = r * 2 / 5, ey = cy - r / 5, er = max<int16_t>(2, r / 7);
  auto eyes = [&](bool open) {
    for (int s : {-1, 1}) {
      if (open) {
        g.fillCircle(cx + s * ex, ey, er, kBlack);
        g.fillCircle(cx + s * ex + er / 3, ey - er / 3, max<int16_t>(1, er / 3), kWhite);  // highlight
      } else {
        g.drawFastHLine(cx + s * ex - er, ey, 2 * er + 1, kBlack);
      }
    }
  };
  const int16_t my = cy + r / 3, mw = r / 3;
  switch (face) {
    case Face::Blink:
    case Face::Sleeping:
      eyes(false);
      g.drawFastHLine(cx - mw / 2, my, mw, kBlack);
      break;
    case Face::Happy:
    case Face::Laughing:
      for (int s : {-1, 1}) {  // smiling eyes
        g.drawLine(cx + s * ex - er, ey + 1, cx + s * ex, ey - er, kBlack);
        g.drawLine(cx + s * ex, ey - er, cx + s * ex + er, ey + 1, kBlack);
      }
      if (face == Face::Laughing) g.fillCircle(cx, my, mw / 2 + 1, kBlack);
      else for (int i = 0; i < 2; i++) g.drawCircleHelper(cx, my - mw / 2 + i, mw, 0x4 | 0x8, kBlack);
      break;
    case Face::Sad:
      eyes(true);
      for (int i = 0; i < 2; i++) g.drawCircleHelper(cx, my + mw / 2 + i, mw / 2 + 2, 0x1 | 0x2, kBlack);
      break;
    case Face::Surprised:
      eyes(true);
      g.drawCircle(cx, my, mw / 3 + 1, kBlack);
      break;
    case Face::Love:
      for (int s : {-1, 1}) {  // heart eyes
        g.fillCircle(cx + s * ex - er / 2, ey - er / 3, er / 2 + 1, kBlack);
        g.fillCircle(cx + s * ex + er / 2, ey - er / 3, er / 2 + 1, kBlack);
        g.fillTriangle(cx + s * ex - er - 1, ey, cx + s * ex + er + 1, ey, cx + s * ex, ey + er + 1, kBlack);
      }
      for (int i = 0; i < 2; i++) g.drawCircleHelper(cx, my - mw / 2 + i, mw, 0x4 | 0x8, kBlack);
      break;
    case Face::Thinking:
      eyes(true);
      g.drawLine(cx - mw / 2, my + 2, cx + mw / 2, my - 1, kBlack);
      g.drawLine(cx - ex - er, ey - er - 4, cx - ex + er, ey - er - 6, kBlack);  // a raised brow
      break;
    case Face::TalkA:
      eyes(true);
      g.fillRoundRect(cx - mw / 2, my - 2, mw, max<int16_t>(4, mw / 2), 2, kBlack);
      break;
    case Face::TalkB:
      eyes(true);
      g.drawFastHLine(cx - mw / 2, my, mw, kBlack);
      break;
    case Face::Listening:
      eyes(true);
      g.drawCircle(cx, my, max<int16_t>(2, mw / 4), kBlack);
      break;
    default:  // Idle
      eyes(true);
      for (int i = 0; i < 2; i++) g.drawCircleHelper(cx, my - mw / 3 + i, mw / 2 + 2, 0x4 | 0x8, kBlack);
  }
}

}  // namespace avatar
