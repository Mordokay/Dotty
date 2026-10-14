#include "wifi_setup.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include <vector>

#include "log.h"
#include "nav_bar.h"
#include "net.h"
#include "ui.h"

namespace wifisetup {
namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

// The network list: 38 px rows, 4 a page, like every list on Dotty.
constexpr int16_t kRowH = 38;
constexpr int kRows = (kW - nav::kHeight - 2) / kRowH;

// The keyboard: 8 columns x 4 rows of 25 x 31 px keys under the nav bar and the password
// field, like a phone's: letters first (QWERTY, each row's overflow moved down to the right
// end of the row below), "123" for digits and punctuation, "#+=" for the rest of ASCII. Join
// is the nav bar's right corner (as on the iPhone's Wi-Fi password screen). Shift is
// one-shot; tapped twice in a row it locks (caps lock).
constexpr int16_t kKeyW = 25, kKeyH = 31;
constexpr int kCols = 8, kKeyRows = 4;
constexpr int16_t kKeysTop = kW - 1 - kKeyRows * kKeyH;  // 75: the last row's bottom edge on the screen's last line
constexpr int16_t kFieldTop = nav::kHeight + 3, kFieldH = kKeysTop - 3 - kFieldTop;
constexpr uint32_t kCapsLockMs = 800;  // shift tapped again within this: caps lock

enum class Kind : uint8_t { Char, Shift, Back, Space, Layer };
enum Layer : uint8_t { kLetters, kNumbers, kSymbols };
struct Key {
  int8_t row, col, span;
  Kind kind;
  char ch;         // Char
  Layer to;        // Layer: the page it opens
  const char *label;
};

std::vector<Key> layout(Layer layer) {
  std::vector<Key> keys;
  auto chars = [&](int row, int col, const char *s) {
    for (int i = 0; s[i]; i++) keys.push_back({int8_t(row), int8_t(col + i), 1, Kind::Char, s[i], kLetters, nullptr});
  };
  auto special = [&](int row, int col, int span, Kind kind, Layer to = kLetters, const char *label = nullptr) {
    keys.push_back({int8_t(row), int8_t(col), int8_t(span), kind, 0, to, label});
  };
  if (layer == kLetters) {
    chars(0, 0, "qwertyui");
    chars(1, 0, "asdfghop");
    chars(2, 0, "zxcvbjkl");
    special(3, 0, 1, Kind::Shift);
    chars(3, 1, "nm");
    special(3, 3, 1, Kind::Layer, kNumbers, "123");
  } else {
    // Digits and the common punctuation; "#+=" has brackets, maths and the rare ones.
    if (layer == kNumbers) {
      chars(0, 0, "12345678");
      chars(1, 0, "90-/:;()");
    } else {
      chars(0, 0, "[]{}#%^*");
      chars(1, 0, "+=\\|<>~`");
    }
    chars(2, 0, "$&@\".,?!");
    special(3, 0, 1, Kind::Layer, layer == kNumbers ? kSymbols : kNumbers, layer == kNumbers ? "#+=" : "123");
    chars(3, 1, "'_");
    special(3, 3, 1, Kind::Layer, kLetters, "ABC");
  }
  special(3, 4, 3, Kind::Space);
  special(3, 7, 1, Kind::Back);
  return keys;
}

constexpr size_t kMaxPassword = 63;           // WPA2's longest passphrase

constexpr uint32_t kConnectedMs = 3000, kFailedMs = 6000;

enum class Step { Closed, Scanning, List, Keyboard, Joining, Joined, Failed };
Step step = Step::Closed;
bool dirty = false;  // the screen needs drawing
bool shown = false;  // the current step's screen has been drawn (scans/joins wait for it)

std::vector<net::Network> found;
int page = 0;
net::Network chosen;
String password;
Layer layer = kLetters;
bool shift = false, capsLock = false;
uint32_t shiftAt = 0;
bool reveal = false;      // the whole password in plain text (tap the field)
bool lastVisible = false; // the character just typed shows until the next key
String error;
uint32_t resultAt = 0;

void go(Step next) {
  step = next;
  shown = false;
  dirty = true;
}

int pages() {
  return max(1, (static_cast<int>(found.size()) + 1 + kRows - 1) / kRows);  // + "Look again"
}

// ---------- drawing ----------

// Four rising bars, filled up to the signal's strength.
void drawSignal(int16_t x, int16_t baseline, int rssi) {
  const int levels = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    const int16_t h = 4 + i * 3, bx = x + i * 4;
    if (i < levels) epd.fillRect(bx, baseline - h, 3, h, kBlack);
    else epd.drawFastHLine(bx, baseline - 1, 3, kBlack);
  }
}

void drawLock(int16_t x, int16_t baseline) {
  epd.drawRoundRect(x + 1, baseline - 12, 6, 7, 2, kBlack);
  epd.fillRect(x, baseline - 7, 8, 7, kBlack);
}

void drawTick(int16_t x, int16_t baseline) {
  for (int d = 0; d < 2; d++) {
    epd.drawLine(x, baseline - 5 + d, x + 3, baseline - 2 + d, kBlack);
    epd.drawLine(x + 3, baseline - 2 + d, x + 9, baseline - 9 + d, kBlack);
  }
}

void drawList() {
  epd.fillScreen(kWhite);
  page = constrain(page, 0, pages() - 1);
  epd.setFont(&FreeSans9pt7b);
  nav::draw(epd, "Wi-Fi", nav::Icon::Back, nav::Icon::None, nav::pageLabel(page, pages()));
  epd.setTextColor(kBlack);
  for (int row = 0; row < kRows; row++) {
    const int i = page * kRows + row;
    if (i > static_cast<int>(found.size())) break;
    const int16_t top = nav::kHeight + 2 + row * kRowH, baseline = top + kRowH / 2 + 6;
    if (i == static_cast<int>(found.size())) {  // the last row: scan again
      epd.setFont(&FreeSansBold9pt7b);
      epd.setCursor(10, baseline);
      epd.print(found.empty() ? "None found - look again" : "Look again");
      break;
    }
    const net::Network &n = found[i];
    // Right side: a tick if Dotty knows it, a padlock if it needs a password, the signal.
    int16_t x = kW - 10 - 15;
    drawSignal(x, baseline, n.rssi);
    x -= 12;
    if (n.secure) drawLock(x, baseline);
    x -= 14;
    if (n.known) drawTick(x, baseline);
    epd.setFont(&FreeSans9pt7b);
    epd.setCursor(10, baseline);
    epd.print(ui::fitText(epd, ui::printable(n.ssid), x - 14));
    epd.drawFastHLine(10, top + kRowH - 1, kW - 20, kBlack);
  }
}

// A key's frame: neighbours share their 1 px border (the right edge stays on the screen).
void keyFrame(const Key &k, bool dark) {
  const int16_t x = k.col * kKeyW, y = kKeysTop + k.row * kKeyH, w = min<int16_t>(k.span * kKeyW + 1, kW - x);
  if (dark) epd.fillRect(x, y, w, kKeyH + 1, kBlack);
  else epd.drawRect(x, y, w, kKeyH + 1, kBlack);
}

// Centred across, on one baseline per row (so "g" and "y" don't jump about).
void keyLabel(const Key &k, const String &label, bool dark) {
  int16_t bx, by;
  uint16_t bw, bh;
  epd.getTextBounds(label, 0, 0, &bx, &by, &bw, &bh);
  const int16_t cx = k.col * kKeyW + k.span * kKeyW / 2, baseline = kKeysTop + k.row * kKeyH + 21;
  epd.setTextColor(dark ? kWhite : kBlack);
  epd.setCursor(cx - bw / 2 - bx, baseline);
  epd.print(label);
  epd.setTextColor(kBlack);
}

void drawKeys() {
  for (const Key &k : layout(layer)) {
    const int16_t cx = k.col * kKeyW + k.span * kKeyW / 2, cy = kKeysTop + k.row * kKeyH + kKeyH / 2;
    switch (k.kind) {
      case Kind::Char:
        keyFrame(k, false);
        epd.setFont(&FreeSans9pt7b);
        keyLabel(k, String(shift && isalpha(k.ch) ? static_cast<char>(toupper(k.ch)) : k.ch), false);
        break;
      case Kind::Shift: {  // an up arrow, dark while on; caps lock adds a bar under it
        keyFrame(k, shift);
        const uint16_t ink = shift ? kWhite : kBlack;
        epd.fillTriangle(cx, cy - 8, cx - 8, cy, cx + 8, cy, ink);
        epd.fillRect(cx - 3, cy, 7, 5, ink);
        if (capsLock) epd.fillRect(cx - 5, cy + 7, 11, 2, ink);
        break;
      }
      case Kind::Back:  // an arrow-shaped tag pointing left with a cross in it
        keyFrame(k, false);
        epd.drawLine(cx - 9, cy, cx - 4, cy - 5, kBlack);
        epd.drawLine(cx - 9, cy, cx - 4, cy + 5, kBlack);
        epd.drawFastHLine(cx - 4, cy - 5, 13, kBlack);
        epd.drawFastHLine(cx - 4, cy + 5, 13, kBlack);
        epd.drawFastVLine(cx + 9, cy - 5, 11, kBlack);
        epd.drawLine(cx - 1, cy - 2, cx + 4, cy + 3, kBlack);
        epd.drawLine(cx - 1, cy + 2, cx + 4, cy - 3, kBlack);
        break;
      case Kind::Space:  // a bracket lying down
        keyFrame(k, false);
        epd.drawFastHLine(cx - 16, cy + 4, 33, kBlack);
        epd.drawFastVLine(cx - 16, cy + 1, 3, kBlack);
        epd.drawFastVLine(cx + 16, cy + 1, 3, kBlack);
        break;
      case Kind::Layer:  // "123" / "ABC" / "#+=": the small built-in font fits 25 px
        keyFrame(k, true);
        epd.setFont(nullptr);
        epd.setTextColor(kWhite);
        epd.setCursor(cx - 8, cy - 3);
        epd.print(k.label);
        epd.setTextColor(kBlack);
        break;
    }
  }
}

// An eye at the field's right end (tap the field to show or hide the password).
void drawEye(int16_t cx, int16_t cy, bool open) {
  for (int dx = -8; dx <= 8; dx++) {
    const float t = dx / 8.0f;
    const int16_t dy = lroundf(5 * (1 - t * t));
    epd.drawPixel(cx + dx, cy - dy, kBlack);
    epd.drawPixel(cx + dx, cy + dy, kBlack);
  }
  epd.fillCircle(cx, cy, 2, kBlack);
  if (!open) {
    epd.drawLine(cx - 8, cy + 6, cx + 8, cy - 6, kBlack);
    epd.drawLine(cx - 8, cy + 7, cx + 8, cy - 5, kWhite);
  }
}

void drawKeyboard() {
  epd.fillScreen(kWhite);
  epd.setFont(&FreeSans9pt7b);
  nav::draw(epd, ui::fitText(epd, ui::printable(chosen.ssid), kW - 2 * nav::kButton - 12), nav::Icon::Back, nav::Icon::None, "Join");
  epd.drawRoundRect(4, kFieldTop, kW - 8, kFieldH, 5, kBlack);
  drawEye(kW - 20, kFieldTop + kFieldH / 2, reveal);
  epd.setTextColor(kBlack);
  const int16_t baseline = kFieldTop + kFieldH / 2 + 6, room = kW - 8 - 36;
  if (password.isEmpty()) {
    epd.setCursor(12, baseline);
    epd.print("Password");
  } else {
    String text = password;
    if (!reveal) {
      text = "";
      for (size_t i = 0; i + 1 < password.length(); i++) text += '*';
      text += lastVisible ? password.substring(password.length() - 1) : String("*");
    }
    // The end of a long password: what was just typed matters most.
    bool cut = false;
    while (ui::textWidth(epd, (cut ? "..." : "") + text) > room) {
      text.remove(0, 1);
      cut = true;
    }
    if (cut) text = "..." + text;
    epd.setCursor(12, baseline);
    epd.print(text);
    const int16_t x = 12 + ui::textWidth(epd, text) + 2;
    epd.fillRect(x, baseline - 13, 2, 16, kBlack);  // the cursor
  }
  // The "Password" placeholder shows the cursor at the start.
  if (password.isEmpty()) epd.fillRect(10, baseline - 13, 2, 16, kBlack);
  drawKeys();
}

void drawMessage(const String &title, const String &line, const String &hint) {
  epd.fillScreen(kWhite);
  epd.setFont(&FreeSans9pt7b);
  nav::draw(epd, "Wi-Fi", nav::Icon::None, nav::Icon::None);
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSansBold9pt7b);
  ui::drawCentered(epd, title, 92);
  epd.setFont(&FreeSans9pt7b);
  ui::drawWrapped(epd, line, 122, kW - 24, 2, 20);
  if (hint.length()) ui::drawCentered(epd, hint, 180);
}

// ---------- actions ----------

void typeKey(char c) {
  if (password.length() >= kMaxPassword) return;
  password += c;
  lastVisible = true;
}

void join() {
  if (chosen.secure && password.length() < 8) {
    error = "Wi-Fi passwords have at least 8 characters.";
    resultAt = millis();
    go(Step::Failed);
    return;
  }
  go(Step::Joining);
}

void choose(const net::Network &n) {
  chosen = n;
  password = "";
  layer = kLetters;
  shift = capsLock = false;
  reveal = false;
  lastVisible = false;
  if (n.secure) go(Step::Keyboard);
  else join();
}

void loopList(const shell::Input &input) {
  using G = Touch::Gesture;
  if (input.boot) return go(Step::Closed);
  if (input.key == 'n' && !found.empty()) return choose(found[0]);  // developer aid
  const int count = pages();
  if (input.gesture == G::SwipeLeft || input.gesture == G::SwipeUp) page = (page + 1) % count, dirty = true;
  if (input.gesture == G::SwipeRight || input.gesture == G::SwipeDown) page = (page + count - 1) % count, dirty = true;
  if (input.gesture != G::Tap) return;
  const uint16_t x = shell::touch.x(), y = shell::touch.y();
  const int corner = nav::hit(x, y);
  if (corner < 0) return go(Step::Closed);
  if (corner > 0) {
    page = (page + 1) % count;
    dirty = true;
    return;
  }
  if (y < nav::kHeight + 2) return;
  const int row = (y - nav::kHeight - 2) / kRowH;
  const int i = page * kRows + row;
  if (row >= kRows || i > static_cast<int>(found.size())) return;
  if (i == static_cast<int>(found.size())) return go(Step::Scanning);
  choose(found[i]);
}

void loopKeyboard(const shell::Input &input) {
  if (input.boot) return go(Step::List);
  // Developer aid: letters from the serial monitor type, ^ = shift, # = next page, = joins (the shell keeps
  // 1-9 etc.).
  if (input.key == '=') return join();
  if (input.key == '^') shift = !shift, dirty = true;
  if (input.key == '#') layer = static_cast<Layer>((layer + 1) % 3), dirty = true;
  else if (isalpha(input.key)) typeKey(input.key), dirty = true;
  if (input.gesture != Touch::Gesture::Tap) return;
  const uint16_t x = shell::touch.x(), y = shell::touch.y();
  dirty = true;
  if (y < nav::kHeight) {
    const int corner = nav::hit(x, y);
    if (corner < 0) go(Step::List);
    else if (corner > 0) join();
    else dirty = false;
    return;
  }
  if (y < kKeysTop) {  // the field: show / hide the password
    reveal = !reveal;
    lastVisible = false;
    return;
  }
  const int row = min(kKeyRows - 1, (y - kKeysTop) / kKeyH), col = min(kCols - 1, x / kKeyW);
  for (const Key &k : layout(layer)) {
    if (k.row != row || col < k.col || col >= k.col + k.span) continue;
    if (k.kind != Kind::Char) lastVisible = false;
    switch (k.kind) {
      case Kind::Char:
        typeKey(shift && isalpha(k.ch) ? toupper(k.ch) : k.ch);
        if (shift && !capsLock) shift = false;
        break;
      case Kind::Shift:
        if (!shift) {
          shift = true;
          shiftAt = millis();
        } else if (!capsLock && millis() - shiftAt < kCapsLockMs) {
          capsLock = true;
        } else {
          shift = capsLock = false;
        }
        break;
      case Kind::Back:
        if (password.length()) password.remove(password.length() - 1);
        break;
      case Kind::Space:
        typeKey(' ');
        break;
      case Kind::Layer:
        layer = k.to;
        shift = capsLock = false;
        break;
    }
    return;
  }
  dirty = false;  // between keys
}

}  // namespace

void open() {
  page = 0;
  go(Step::Scanning);
}

bool active() {
  return step != Step::Closed;
}

bool loop(const shell::Input &input) {
  switch (step) {
    case Step::Closed:
      return false;
    case Step::Scanning:
      // Scanning blocks for a few seconds: only once "Looking for networks" is on screen.
      if (shown && !epd.isBusy()) {
        found = net::scan();
        LOGI("wifi", "setup: %u networks in range", static_cast<unsigned>(found.size()));
        page = 0;
        go(Step::List);
      }
      break;
    case Step::List:
      loopList(input);
      break;
    case Step::Keyboard:
      loopKeyboard(input);
      break;
    case Step::Joining:
      if (shown && !epd.isBusy()) {
        error = "";
        bool ok = net::connectTo(chosen.ssid, password, error);
        if (ok && !net::remember(chosen.ssid, password)) {
          ok = false;
          error = "Dotty already knows 8 networks. Remove one in the app.";
        }
        net::disconnect();
        LOGI("wifi", "setup: joining '%s' %s", chosen.ssid.c_str(), ok ? "worked" : error.c_str());
        // A wrong password usually looks like "cannot join" (the router just stops answering).
        if (!ok && (error.startsWith("cannot join") || error.startsWith("wrong password"))) {
          error = chosen.secure ? "Check the password. Is Dotty close to the router?" : "Is Dotty close to the router?";
        }
        resultAt = millis();
        go(ok ? Step::Joined : Step::Failed);
      }
      break;
    case Step::Joined:
      if (millis() - resultAt > kConnectedMs || input.gesture == Touch::Gesture::Tap || input.boot) go(Step::Closed);
      break;
    case Step::Failed:
      // Back to the password (kept, so a typo can be fixed), or the list for an open network.
      if (millis() - resultAt > kFailedMs || input.gesture == Touch::Gesture::Tap || input.boot) {
        go(chosen.secure ? Step::Keyboard : Step::List);
      }
      break;
  }
  if (!active()) return false;
  if (!dirty || epd.isBusy()) return false;
  dirty = false;
  return true;
}

void draw() {
  switch (step) {
    case Step::Closed:
      break;
    case Step::Scanning:
      drawMessage("Looking for networks", "This takes a few seconds.", "");
      break;
    case Step::List:
      drawList();
      break;
    case Step::Keyboard:
      drawKeyboard();
      break;
    case Step::Joining:
      drawMessage("Joining...", ui::printable(chosen.ssid), "");
      break;
    case Step::Joined:
      drawMessage("Connected!", ui::printable(chosen.ssid) + " is saved.", "");
      break;
    case Step::Failed:
      drawMessage("Couldn't join", error, "Tap to try again");
      break;
  }
  shown = true;
  dirty = false;
}

}  // namespace wifisetup
