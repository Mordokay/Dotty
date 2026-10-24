#include "screens.h"
#include "text_pages.h"

#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include "avatar.h"
#include "nav_bar.h"
#include "shell.h"
#include "ui.h"

namespace screens {
namespace {

using shell::epd;
constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack, kWhite = EpdDisplay::kWhite;
constexpr int16_t kLineH = 19;

// Settings (top left): a gear.
void drawGear() {
  const int16_t cx = 17, cy = 15;
  for (int i = 0; i < 8; i++) {  // teeth
    const float a = i * PI / 4;
    epd.fillCircle(cx + roundf(cosf(a) * 9), cy + roundf(sinf(a) * 9), 2, kBlack);
  }
  epd.fillCircle(cx, cy, 8, kBlack);
  epd.fillCircle(cx, cy, 3, kWhite);
}

// History (top right): a clock face with a back arrow.
void drawHistoryIcon() {
  const int16_t cx = kW - 18, cy = 15;
  epd.drawCircle(cx, cy, 9, kBlack);
  epd.drawCircle(cx, cy, 8, kBlack);
  epd.drawFastVLine(cx, cy - 5, 6, kBlack);
  epd.drawLine(cx, cy, cx + 4, cy + 3, kBlack);
}

// A moment's kind, top right (where the history clock usually is).
void drawMomentIcon(const String &kind) {
  const int16_t cx = kW - 18, cy = 15;
  if (kind == "alarm") {  // a clock with two bells on top
    epd.fillCircle(cx - 7, cy - 8, 4, kBlack);
    epd.fillCircle(cx + 7, cy - 8, 4, kBlack);
    epd.fillCircle(cx, cy + 1, 10, kBlack);
    epd.fillCircle(cx, cy + 1, 7, kWhite);
    epd.drawFastVLine(cx, cy - 4, 6, kBlack);
    epd.drawLine(cx, cy + 1, cx + 4, cy + 4, kBlack);
  } else if (kind == "timer") {  // an hourglass
    epd.drawFastHLine(cx - 8, cy - 11, 17, kBlack);
    epd.drawFastHLine(cx - 8, cy + 11, 17, kBlack);
    epd.drawTriangle(cx - 7, cy - 10, cx + 7, cy - 10, cx, cy, kBlack);
    epd.fillTriangle(cx - 7, cy + 10, cx + 7, cy + 10, cx, cy, kBlack);
  } else if (kind == "speech") {  // a speech bubble
    epd.fillRoundRect(cx - 11, cy - 9, 22, 15, 5, kBlack);
    epd.fillTriangle(cx - 6, cy + 5, cx - 1, cy + 5, cx - 8, cy + 11, kBlack);
  } else {  // a bell
    epd.fillCircle(cx, cy - 4, 7, kBlack);
    epd.fillRect(cx - 7, cy - 4, 15, 9, kBlack);
    epd.fillRoundRect(cx - 10, cy + 4, 21, 3, 1, kBlack);
    epd.fillCircle(cx, cy + 9, 2, kBlack);
    epd.fillRect(cx - 1, cy - 13, 3, 3, kBlack);
  }
}

// The replay buttons in a column under the gear, left of the avatar (the bottom third stays for the
// text and the meter): ▶ / ‖, then loop (dark when on).
constexpr int16_t kReplaySize = 30, kReplayX = 3, kPlayY = 40, kLoopY = 76;

void drawReplayButtons(bool playing, bool loop) {
  epd.drawRoundRect(kReplayX, kPlayY, kReplaySize, kReplaySize, 6, kBlack);
  const int16_t cx = kReplayX + kReplaySize / 2, cy = kPlayY + kReplaySize / 2;
  if (playing) {  // pause
    epd.fillRect(cx - 6, cy - 7, 4, 14, kBlack);
    epd.fillRect(cx + 2, cy - 7, 4, 14, kBlack);
  } else {  // play
    epd.fillTriangle(cx - 4, cy - 8, cx - 4, cy + 8, cx + 7, cy, kBlack);
  }
  const uint16_t ink = loop ? kWhite : kBlack;
  if (loop) epd.fillRoundRect(kReplayX, kLoopY, kReplaySize, kReplaySize, 6, kBlack);
  else epd.drawRoundRect(kReplayX, kLoopY, kReplaySize, kReplaySize, 6, kBlack);
  const int16_t lx = kReplayX + kReplaySize / 2, ly = kLoopY + kReplaySize / 2;
  // A ring with a gap at the top right and an arrowhead: "again".
  for (int r = 7; r <= 9; r++) epd.drawCircle(lx, ly, r, ink);
  epd.fillRect(lx + 1, ly - 10, 10, 8, loop ? kBlack : kWhite);
  epd.fillTriangle(lx + 1, ly - 13, lx + 1, ly - 3, lx + 7, ly - 8, ink);
}

// A note over the avatar's lower part (a memory saved, "Thinking back...", "Saving your reminder..."): a
// bookmark (or a pencil, for a long answer being written) and the words in the 9 pt font, a white card
// over the avatar — 2x the first small one, which the user could barely read (2.5x was too big). Two
// lines when needed.
void drawNote(const String &note, bool small, bool pencil = false) {
  (void)small;  // the same card in text-only mode: legibility first
  constexpr int16_t kIcon = 20, kPad = 7, kLine = 19;
  epd.setFont(&FreeSans9pt7b);
  const int16_t textW = kW - 2 * 4 - 2 * kPad - kIcon - 6;
  const std::vector<String> lines = ui::wrapText(epd, ui::printable(note), textW, 2);
  int16_t widest = 0;
  for (const String &l : lines) {
    int16_t bx, by;
    uint16_t w, h;
    epd.getTextBounds(l, 0, 0, &bx, &by, &w, &h);
    widest = max<int16_t>(widest, w);
  }
  const int16_t boxW = kPad + kIcon + 6 + widest + kPad, boxH = 12 + kLine * lines.size();
  const int16_t x = (kW - boxW) / 2, y = 132 - boxH;  // its bottom at the avatar's bottom
  epd.fillRoundRect(x, y, boxW, boxH, 8, kWhite);
  epd.drawRoundRect(x, y, boxW, boxH, 8, kBlack);
  epd.drawRoundRect(x + 1, y + 1, boxW - 2, boxH - 2, 7, kBlack);
  const int16_t ix = x + kPad, iy = y + (boxH - 24) / 2;
  if (pencil) {
    for (int d = -2; d <= 1; d++) epd.drawLine(ix + 4 + d, iy + 19, ix + 18 + d, iy + 5, kBlack);  // the body
    epd.fillTriangle(ix, iy + 24, ix + 2, iy + 17, ix + 7, iy + 22, kBlack);                      // the tip
  } else {  // a bookmark
    epd.fillRect(ix + 2, iy, 17, 24, kBlack);
    epd.fillTriangle(ix + 2, iy + 24, ix + 18, iy + 24, ix + 10, iy + 15, kWhite);
  }
  epd.setTextColor(kBlack);
  for (size_t i = 0; i < lines.size(); i++) {
    epd.setCursor(ix + kIcon + 6, y + 6 + 14 + i * kLine);
    epd.print(lines[i]);
  }
}

void drawMeter(int levelDb, int16_t y) {
  // 20 bars from -60 to 0 dBFS, like Tape's.
  const int bars = constrain((levelDb + 60) / 3, 0, 20);
  for (int i = 0; i < 20; i++) {
    const int16_t x = 30 + i * 7, h = 4 + i / 2;
    if (i < bars) epd.fillRect(x, y - h, 5, h, kBlack);
    else epd.drawFastHLine(x, y - 1, 5, kBlack);
  }
}

std::vector<String> wrap(const String &text, int16_t width, int maxLines) {
  epd.setFont(&FreeSans9pt7b);
  return ui::wrapText(epd, ui::printable(text), width, maxLines);
}

}  // namespace

namespace {

void centeredIn(const String &text, int16_t y, int16_t x0, int16_t x1) {
  int16_t bx, by;
  uint16_t w, h;
  epd.getTextBounds(text, 0, 0, &bx, &by, &w, &h);
  epd.setCursor(x0 + (x1 - x0 - w) / 2 - bx, y);
  epd.print(text);
}

// A running timer: an hourglass and its label, the time left big, a hint (or the cancel question).
void drawTimer(const ScreenInfo &info) {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  nav::draw(epd, ui::printable(info.timerLabel), nav::Icon::Back, nav::Icon::None);  // back: the companion
  epd.setFont(&FreeSansBold24pt7b);
  ui::drawCentered(epd, info.timerClock, 108);
  epd.setFont(&FreeSans9pt7b);
  if (info.timerConfirm) {
    ui::drawCentered(epd, "Cancel the timer?", 142);
    epd.fillRoundRect(12, 154, 80, 36, 6, kBlack);
    epd.drawRoundRect(108, 154, 80, 36, 6, kBlack);
    epd.setTextColor(kWhite);
    centeredIn("Yes", 177, 12, 92);
    epd.setTextColor(kBlack);
    centeredIn("No", 177, 108, 188);
    return;
  }
  ui::drawCentered(epd, info.timerPaused ? "Paused - tap to go on" : "Tap: pause", 150);
  ui::drawCentered(epd, "Hold the screen: cancel", 176);
}

}  // namespace

// The recording deck: the title in the nav bar (back = cancel), REC or PAUSED with the time, the meter
// while recording, Undo · Done · Discard while paused, or the discard question.
void drawDeck(const DeckView &v) {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  nav::draw(epd, ui::printable(v.title), nav::Icon::Back, nav::Icon::None);
  if (v.confirm) {
    epd.setFont(&FreeSans12pt7b);
    ui::drawCentered(epd, "Discard this", 92);
    ui::drawCentered(epd, "recording?", 118);
    epd.fillRoundRect(12, 150, 80, 40, 6, kBlack);
    epd.drawRoundRect(108, 150, 80, 40, 6, kBlack);
    epd.setFont(&FreeSans9pt7b);
    epd.setTextColor(kWhite);
    centeredIn("Yes", 175, 12, 92);
    epd.setTextColor(kBlack);
    centeredIn("No", 175, 108, 188);
    return;
  }
  if (v.recording) {
    epd.fillCircle(52, 84, 9, kBlack);
    epd.setFont(&FreeSansBold24pt7b);
    epd.setCursor(70, 100);
    epd.print("REC");
  } else if (v.empty) {
    epd.setFont(&FreeSans12pt7b);
    ui::drawCentered(epd, "Hold BOOT", 88);
    ui::drawCentered(epd, "to record", 114);
  } else {
    epd.setFont(&FreeSans12pt7b);
    ui::drawCentered(epd, "PAUSED", 90);
  }
  if (!v.empty || v.recording) {
    epd.setFont(&FreeSansBold12pt7b);
    ui::drawCentered(epd, v.clock, 128);
  }
  if (v.recording) {  // 20 bars, -60..0 dBFS, like Tape's
    const int bars = constrain((v.levelDb + 60) / 3, 0, 20);
    for (int i = 0; i < 20; i++) {
      const int16_t x = 30 + i * 7, h = 4 + i / 2;
      if (i < bars) epd.fillRect(x, 170 - h, 5, h, kBlack);
      else epd.drawFastHLine(x, 169, 5, kBlack);
    }
    return;
  }
  if (v.paused && !v.empty) {  // ↶ undo · ✓ done (dark) · trash
    epd.drawRoundRect(4, 150, 60, 44, 6, kBlack);
    epd.fillRoundRect(70, 150, 60, 44, 6, kBlack);
    epd.drawRoundRect(136, 150, 60, 44, 6, kBlack);
    for (int r = 9; r <= 10; r++) epd.drawCircle(34, 172, r, kBlack);  // undo: a ring open at the top left, an arrowhead
    epd.fillRect(22, 160, 12, 12, kWhite);
    epd.fillTriangle(20, 166, 30, 158, 30, 172, kBlack);
    for (int d = 0; d < 3; d++) {  // done: a tick
      epd.drawLine(88, 171 + d, 96, 179 + d, kWhite);
      epd.drawLine(96, 179 + d, 112, 163 + d, kWhite);
    }
    epd.drawRect(158, 164, 16, 18, kBlack);  // discard: a bin
    epd.drawFastHLine(154, 162, 24, kBlack);
    epd.fillRect(162, 158, 8, 4, kBlack);
    for (int x = 162; x <= 170; x += 4) epd.drawFastVLine(x, 167, 12, kBlack);
  }
}

DeckHit deckHit(uint16_t x, uint16_t y, const DeckView &v) {
  if (nav::hit(x, y) < 0) return DeckHit::Back;
  if (y < 146) return DeckHit::None;
  if (v.confirm) return x < kW / 2 ? DeckHit::Yes : DeckHit::No;
  if (!v.paused || v.empty) return DeckHit::None;
  return x < 67 ? DeckHit::Undo : x < 133 ? DeckHit::Done : DeckHit::Discard;
}

// The recordings list: like Dotty's other lists (38 px rows, 4 a page, the page in the nav's right corner).
void drawRecordings(const RecordingsView &v) {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  if (v.playing) {  // the one playing: title, when and how long, its summary; a tap stops it
    nav::draw(epd, "Playing", nav::Icon::Back, nav::Icon::None);
    epd.setFont(&FreeSansBold9pt7b);
    int16_t y = nav::kHeight + 20;
    for (const String &l : ui::wrapText(epd, ui::printable(v.now.title), kW - 16, 2)) {
      ui::drawCentered(epd, l, y);
      y += kLineH;
    }
    epd.setFont(&FreeSans9pt7b);
    ui::drawCentered(epd, v.now.detail, y);
    y += 6;
    epd.drawFastHLine(8, y, kW - 16, kBlack);
    y += kLineH;
    const int lines = max(0, (kW - 26 - y) / kLineH + 1);
    for (const String &l : ui::wrapText(epd, ui::printable(v.summary), kW - 16, lines)) {
      epd.setCursor(8, y);
      epd.print(l);
      y += kLineH;
    }
    ui::drawCentered(epd, "Tap to stop", kW - 6);
    return;
  }
  nav::draw(epd, "Recordings", nav::Icon::Back, nav::Icon::None, nav::pageLabel(v.page, v.pages));
  for (size_t i = 0; i < v.rows.size() && i < 4; i++) {
    const int16_t top = nav::kHeight + i * 38;
    epd.setFont(&FreeSansBold9pt7b);
    const std::vector<String> title = ui::wrapText(epd, ui::printable(v.rows[i].title), kW - 16, 1);
    epd.setCursor(8, top + 15);
    if (!title.empty()) epd.print(title[0]);
    epd.setFont(&FreeSans9pt7b);
    epd.setCursor(8, top + 31);
    epd.print(v.rows[i].detail);
    if (i + 1 < v.rows.size()) epd.drawFastHLine(8, top + 37, kW - 16, kBlack);
  }
}

RecordingsHit recordingsHit(uint16_t x, uint16_t y, int rows, int &row) {
  if (y < nav::kHeight) {
    const int corner = nav::hit(x, y);
    return corner < 0 ? RecordingsHit::Back : corner > 0 ? RecordingsHit::NextPage : RecordingsHit::None;
  }
  row = (y - nav::kHeight) / 38;
  return row < rows ? RecordingsHit::Row : RecordingsHit::None;
}

TimerHit timerHit(uint16_t x, uint16_t y) {
  if (nav::hit(x, y) < 0) return TimerHit::Back;
  if (y < 148) return TimerHit::None;
  return x < kW / 2 ? TimerHit::Yes : TimerHit::No;
}

// Four rows of 38 px under the nav bar: Companion ‹ › / Volume - + / Speak switch / Voice ‹ ›.
namespace {
constexpr int16_t kRow = 38, kRow0 = 45;  // rows at 45, 83, 121, 159

void arrows(int16_t top) {
  for (int i = 0; i < 2; i++) {
    const int16_t cx = i ? kW - 14 : 14, cy = top + kRow / 2;
    const int16_t d = i ? 5 : -5;  // the tip points outwards
    epd.fillTriangle(cx - d, cy - 8, cx - d, cy + 8, cx + d, cy, kBlack);
  }
}

// A bold line and a small one under it, centred between the arrows.
void stepper(int16_t top, const String &big, const String &small, bool showArrows) {
  if (showArrows) arrows(top);
  epd.setFont(&FreeSansBold9pt7b);
  ui::drawCentered(epd, big, top + 18);
  epd.setFont(nullptr);
  epd.setCursor((kW - 6 * small.length()) / 2, top + 25);
  epd.print(small);
}
}  // namespace

void drawSettings(const SettingsView &v) {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  nav::draw(epd, "Settings", nav::Icon::Back, nav::Icon::None);
  for (int r = 1; r < 4; r++) epd.drawFastHLine(8, kRow0 + r * kRow, kW - 16, kBlack);

  stepper(kRow0, v.companion, v.companionType, v.companions > 1);

  const int16_t vol = kRow0 + kRow;
  epd.setFont(&FreeSansBold9pt7b);
  epd.setCursor(8, vol + 24);
  epd.print("Volume");
  epd.setFont(&FreeSans9pt7b);
  epd.print(" " + String(v.volume) + "%");
  for (int i = 0; i < 2; i++) {  // - and + buttons
    const int16_t x = i ? 162 : 124;
    epd.drawRoundRect(x, vol + 5, 34, 28, 5, kBlack);
    epd.fillRect(x + 10, vol + 18, 14, 3, kBlack);
    if (i) epd.fillRect(x + 16, vol + 12, 3, 15, kBlack);
  }

  const int16_t speak = kRow0 + 2 * kRow;
  epd.setFont(&FreeSansBold9pt7b);
  epd.setCursor(8, speak + 24);
  epd.print("Speak");
  epd.setFont(&FreeSans9pt7b);
  epd.print(v.speak ? " out loud" : " text only");
  epd.drawRoundRect(150, speak + 7, 44, 24, 12, kBlack);  // a switch: knob right and filled when on
  if (v.speak) {
    epd.fillRoundRect(150, speak + 7, 44, 24, 12, kBlack);
    epd.fillCircle(182, speak + 19, 8, kWhite);
  } else {
    epd.drawCircle(162, speak + 19, 8, kBlack);
  }

  stepper(kRow0 + 3 * kRow, v.voice, v.style, v.choices != 1);  // 0 favourites = every voice
}

SettingsHit settingsHit(uint16_t x, uint16_t y) {
  if (y < nav::kHeight) return nav::hit(x, y) < 0 ? SettingsHit::Back : SettingsHit::None;
  const int row = (y - kRow0) / kRow;
  const bool left = x < 70, right = x > kW - 70;
  switch (row) {
    case 0: return left ? SettingsHit::CompanionPrev : right ? SettingsHit::CompanionNext : SettingsHit::None;
    case 1: return x >= 158 ? SettingsHit::VolumeUp : x >= 116 ? SettingsHit::VolumeDown : SettingsHit::None;
    case 2: return SettingsHit::Speak;
    default: return left ? SettingsHit::VoicePrev : right ? SettingsHit::VoiceNext : SettingsHit::None;
  }
}

int replyPages(const ScreenInfo &info) {
  const bool small = info.textOnly;
  return pageCount(wrap(info.said, small ? kW - 70 : kW - 16, 40).size(), small ? 8 : 2);
}

ReplayHit replayHit(uint16_t x, uint16_t y) {
  if (x > kReplayX + kReplaySize + 4 || y < kPlayY) return ReplayHit::None;  // above: the gear
  if (y < (kPlayY + kReplaySize + kLoopY) / 2) return ReplayHit::PlayPause;
  if (y < kLoopY + kReplaySize + 8) return ReplayHit::Loop;
  return ReplayHit::None;
}

void draw(const ScreenInfo &shownInfo) {
  // A replay looks like the reply did: talking while it plays, its text paused.
  if (shownInfo.timerClock.length()) return drawTimer(shownInfo);
  ScreenInfo info = shownInfo;
  if (info.replaying) info.state = Talk::Speaking;
  else if (info.replayPaused) info.state = Talk::After;
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  const bool small = info.textOnly && (info.state == Talk::Speaking || info.state == Talk::After);
  // The avatar: centred and large, or small in the corner when text takes the screen.
  const avatar::Face face = avatar::faceFor(info.state, info.mood, info.frame);
  if (small) avatar::draw(epd, kW - 52, 30, info.avatar, face, info.frame, 48);
  else avatar::draw(epd, (kW - kAvatarSize) / 2, 2, info.avatar, face, info.frame, kAvatarSize);
  drawGear();
  if (info.momentIcon.length()) {
    drawMomentIcon(info.momentIcon);
  } else if (info.timerChip.length()) {  // a running timer: its time left (a tap opens it); history below
    epd.setFont(nullptr);
    const int16_t w = 6 * info.timerChip.length() + 16;
    epd.drawRoundRect(kW - w - 3, 4, w, 20, 5, kBlack);
    const int16_t hx = kW - w + 3;
    epd.drawFastHLine(hx, 8, 7, kBlack);
    epd.drawFastHLine(hx, 19, 7, kBlack);
    epd.drawTriangle(hx, 9, hx + 6, 9, hx + 3, 13, kBlack);
    epd.fillTriangle(hx, 18, hx + 6, 18, hx + 3, 14, kBlack);
    epd.setCursor(hx + 10, 10);
    epd.print(info.timerChip);
    const int16_t cx = kW - 18, cy = 42;  // the history clock, moved down
    epd.drawCircle(cx, cy, 9, kBlack);
    epd.drawCircle(cx, cy, 8, kBlack);
    epd.drawFastVLine(cx, cy - 5, 6, kBlack);
    epd.drawLine(cx, cy, cx + 4, cy + 3, kBlack);
  } else {
    drawHistoryIcon();
  }
  if (info.replayable && !small) drawReplayButtons(info.replaying, info.replayLoop);
  if ((shownInfo.replaying || shownInfo.replayPaused) && !small) {  // how far the replay is, like a song's bar
    constexpr int16_t x = 8, y = 135, w = kW - 16, h = 6;
    epd.drawRoundRect(x, y, w, h, 3, kBlack);
    const int16_t filled = static_cast<int32_t>(w - 4) * info.replayProgress / 1000;
    if (filled > 0) epd.fillRect(x + 2, y + 2, filled, h - 4, kBlack);
  }
  // The note last: over the avatar and the replay buttons.
  if (info.writing) drawNote("Writing...", small, true);
  else if (info.recalling) drawNote("Thinking back...", small);
  else if (info.note.length() && info.state != Talk::Listening) drawNote(info.note, small);

  epd.setFont(&FreeSans9pt7b);
  // Below the 128 px avatar: two lines of text (baselines 158 and 177), the meter between them.
  constexpr int16_t kText = 158;
  int16_t y = small ? 50 : kText;
  switch (info.state) {
    case Talk::Listening: {
      drawMeter(info.levelDb, 144);
      const std::vector<String> lines = wrap(info.heard.length() ? info.heard : String("Listening..."), kW - 16, 2);
      for (size_t i = 0; i < lines.size(); i++) ui::drawCentered(epd, lines[i], 163 + i * kLineH);
      return;
    }
    case Talk::Speaking:
    case Talk::After: {
      const int16_t width = small ? kW - 70 : kW - 16;
      const int visible = small ? 8 : 2;
      std::vector<String> lines = wrap(info.said, width, 40);
      const int n = lines.size();
      const int page = info.state == Talk::Speaking ? livePage(n, visible) : shownPage(info.page, n, visible);
      for (int i = 0; i < visible && page * visible + i < n; i++) {
        epd.setCursor(8, (small ? 50 : kText) + i * kLineH);
        epd.print(lines[page * visible + i]);
      }
      const int pages = pageCount(n, visible);
      if (info.state == Talk::After && pages > 1) {  // swipe up / down for the others
        const String label = String(page + 1) + "/" + String(pages);
        epd.setFont(nullptr);
        epd.setCursor(kW - 6 - 6 * label.length(), kW - 10);
        epd.print(label);
      }
      return;
    }
    case Talk::Thinking:
      if (info.heard.length()) {
        const std::vector<String> lines = wrap(info.heard, kW - 16, 2);
        for (size_t i = 0; i < lines.size(); i++) ui::drawCentered(epd, lines[i], kText + i * kLineH);
        return;
      }
      break;
    default:
      break;
  }
  const std::vector<String> lines = wrap(info.status, kW - 16, 2);
  for (size_t i = 0; i < lines.size(); i++) ui::drawCentered(epd, lines[i], y + i * kLineH);
}

void drawHistory(const std::vector<std::pair<String, String>> &exchanges, int scroll) {
  epd.fillScreen(kWhite);
  epd.setFont(&FreeSans9pt7b);
  nav::draw(epd, "Today", nav::Icon::Back, nav::Icon::None);
  epd.setTextColor(kBlack);
  std::vector<std::pair<bool, String>> lines;  // (bold = you, text)
  for (const auto &e : exchanges) {
    epd.setFont(&FreeSansBold9pt7b);
    for (const String &l : ui::wrapText(epd, ui::printable(e.first), kW - 16, 20)) lines.push_back({true, l});
    epd.setFont(&FreeSans9pt7b);
    for (const String &l : ui::wrapText(epd, ui::printable(e.second), kW - 16, 20)) lines.push_back({false, l});
  }
  if (lines.empty()) {
    ui::drawCentered(epd, "No chats today", 110);
    return;
  }
  const int visible = 7;
  // Newest at the bottom; scroll counts lines up from there.
  const int first = constrain(static_cast<int>(lines.size()) - visible - scroll, 0, max(0, static_cast<int>(lines.size()) - visible));
  for (int i = 0; i < visible && first + i < static_cast<int>(lines.size()); i++) {
    epd.setFont(lines[first + i].first ? &FreeSansBold9pt7b : &FreeSans9pt7b);
    epd.setCursor(8, nav::kHeight + 20 + i * kLineH);
    epd.print(lines[first + i].second);
  }
}

}  // namespace screens
