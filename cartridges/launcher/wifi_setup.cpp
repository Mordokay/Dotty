#include "wifi_setup.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include <vector>

#include "keyboard.h"
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

// The password screen: the nav bar (back · network · "Join" in the right corner, as on the
// iPhone's Wi-Fi password screen), the password field, the keyboard (keyboard.h).
constexpr int16_t kFieldTop = nav::kHeight + 3, kFieldH = keyboard::kTop - 3 - kFieldTop;

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
  keyboard::draw();
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
  keyboard::reset();
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
  // Developer aid: letters from the serial monitor type, ^ = shift, # = next page, = joins (the
  // shell keeps 1-9 etc.).
  if (input.key == '=') return join();
  keyboard::Result result = keyboard::key(input.key, password, kMaxPassword);
  if (input.gesture == Touch::Gesture::Tap) {
    const uint16_t x = shell::touch.x(), y = shell::touch.y();
    if (y < nav::kHeight) {
      const int corner = nav::hit(x, y);
      if (corner < 0) return go(Step::List);
      if (corner > 0) return join();
    } else if (y < keyboard::kTop) {  // the field: show / hide the password
      reveal = !reveal;
      lastVisible = false;
      dirty = true;
    } else {
      result = keyboard::tap(x, y, password, kMaxPassword);
    }
  }
  if (result == keyboard::Result::None) return;
  lastVisible = result == keyboard::Result::Typed;  // the character just typed shows until the next key
  dirty = true;
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
