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

void drawSettings(const SettingsView &v) {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  nav::draw(epd, "Settings", nav::Icon::Back, nav::Icon::None);
  epd.drawFastHLine(8, 97, kW - 16, kBlack);
  epd.drawFastHLine(8, 149, kW - 16, kBlack);

  epd.setFont(&FreeSansBold9pt7b);
  epd.setCursor(8, 68);
  epd.print("Volume");
  epd.setFont(&FreeSans9pt7b);
  epd.setCursor(8, 88);
  epd.print(String(v.volume) + "%");
  for (int i = 0; i < 2; i++) {  // - and + buttons
    const int16_t x = i ? 154 : 104;
    epd.drawRoundRect(x, 55, 40, 34, 5, kBlack);
    epd.fillRect(x + 12, 71, 16, 3, kBlack);
    if (i) epd.fillRect(x + 18, 64, 3, 16, kBlack);
  }

  epd.setFont(&FreeSansBold9pt7b);
  epd.setCursor(8, 120);
  epd.print("Speak");
  epd.setFont(&FreeSans9pt7b);
  epd.setCursor(8, 140);
  epd.print(v.speak ? "Out loud" : "Text only");
  // A switch: knob right and filled when on.
  epd.drawRoundRect(146, 111, 46, 26, 13, kBlack);
  if (v.speak) {
    epd.fillRoundRect(146, 111, 46, 26, 13, kBlack);
    epd.fillCircle(179, 124, 9, kWhite);
  } else {
    epd.drawCircle(159, 124, 9, kBlack);
  }

  // Voice: < name > with Google's description under it.
  for (int i = 0; i < 2 && v.choices != 1; i++) {  // 0 favourites = every voice
    const int16_t cx = i ? kW - 18 : 18;
    const int16_t d = i ? 5 : -5;  // the tip points outwards
    epd.fillTriangle(cx - d, 166, cx - d, 184, cx + d, 175, kBlack);
  }
  epd.setFont(&FreeSansBold9pt7b);
  ui::drawCentered(epd, v.voice, 172);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, v.style, 192);
}

SettingsHit settingsHit(uint16_t x, uint16_t y) {
  if (y < nav::kHeight) return nav::hit(x, y) < 0 ? SettingsHit::Back : SettingsHit::None;
  if (y < 98) return x >= 150 ? SettingsHit::VolumeUp : x >= 96 ? SettingsHit::VolumeDown : SettingsHit::None;
  if (y < 150) return SettingsHit::Speak;
  return x < 70 ? SettingsHit::VoicePrev : x > kW - 70 ? SettingsHit::VoiceNext : SettingsHit::None;
}

int replyPages(const ScreenInfo &info) {
  const bool small = info.textOnly;
  return pageCount(wrap(info.said, small ? kW - 70 : kW - 16, 40).size(), small ? 8 : 3);
}

void draw(const ScreenInfo &info) {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  const bool small = info.textOnly && (info.state == Talk::Speaking || info.state == Talk::After);
  // The avatar: centred and large, or small in the corner when text takes the screen.
  const avatar::Face face = avatar::faceFor(info.state, info.mood, info.frame);
  if (small) avatar::draw(epd, kW - 52, 30, face, info.frame, 48);
  else avatar::draw(epd, (kW - kAvatarSize) / 2, 8, face, info.frame, kAvatarSize);
  drawGear();
  drawHistoryIcon();

  epd.setFont(&FreeSans9pt7b);
  int16_t y = small ? 34 : 124;
  switch (info.state) {
    case Talk::Listening: {
      drawMeter(info.levelDb, 128);
      const std::vector<String> lines = wrap(info.heard.length() ? info.heard : String("Listening..."), kW - 16, 3);
      for (size_t i = 0; i < lines.size(); i++) ui::drawCentered(epd, lines[i], 148 + i * kLineH);
      return;
    }
    case Talk::Speaking:
    case Talk::After: {
      const int16_t width = small ? kW - 70 : kW - 16;
      const int visible = small ? 8 : 3;
      std::vector<String> lines = wrap(info.said, width, 40);
      const int n = lines.size();
      const int page = info.state == Talk::Speaking ? livePage(n, visible) : shownPage(info.page, n, visible);
      for (int i = 0; i < visible && page * visible + i < n; i++) {
        epd.setCursor(8, (small ? 50 : 140) + i * kLineH);
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
        const std::vector<String> lines = wrap(info.heard, kW - 16, 3);
        for (size_t i = 0; i < lines.size(); i++) ui::drawCentered(epd, lines[i], 140 + i * kLineH);
        return;
      }
      break;
    default:
      break;
  }
  const std::vector<String> lines = wrap(info.status, kW - 16, 3);
  for (size_t i = 0; i < lines.size(); i++) ui::drawCentered(epd, lines[i], y + 16 + i * kLineH);
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
