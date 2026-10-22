#include "screens.h"
#include "text_pages.h"

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

// A memory change: a bookmark and the word, under the avatar (beside it in text-only mode).
void drawNote(const String &note, bool small) {
  epd.setFont(nullptr);  // the small built-in font: 6 px a character
  const int16_t w = 10 + 4 + 6 * note.length();
  const int16_t x = small ? kW - 6 - w : (kW - w) / 2, y = small ? 82 : 116;
  epd.fillRoundRect(x - 4, y - 2, w + 8, 16, 4, kWhite);  // a white strip over the avatar's edge
  epd.drawRoundRect(x - 4, y - 2, w + 8, 16, 4, kBlack);
  epd.fillRect(x, y, 10, 12, kBlack);
  epd.fillTriangle(x, y + 12, x + 9, y + 12, x + 5, y + 7, kWhite);
  epd.setTextColor(kBlack);
  epd.setCursor(x + 14, y + 3);
  epd.print(note);
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

void draw(const ScreenInfo &info) {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  const bool small = info.textOnly && (info.state == Talk::Speaking || info.state == Talk::After);
  // The avatar: centred and large, or small in the corner when text takes the screen.
  const avatar::Face face = avatar::faceFor(info.state, info.mood, info.frame);
  if (small) avatar::draw(epd, kW - 52, 30, info.avatar, face, info.frame, 48);
  else avatar::draw(epd, (kW - kAvatarSize) / 2, 2, info.avatar, face, info.frame, kAvatarSize);
  drawGear();
  if (info.note.length() && info.state != Talk::Listening) drawNote(info.note, small);
  drawHistoryIcon();

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
