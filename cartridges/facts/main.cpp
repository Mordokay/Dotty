// Facts: ten thousand mind-blowing, dinner-table facts in 20 categories, built in (facts_store.h),
// so they never need the internet or an update. Like News, but it never gets old.
//
// Screens: the menu (a row of three buttons on top - Random: facts from every category, Play:
// the same as a shuffled playlist that moves on by itself, Favourites: the starred ones - then
// each category with how many are still new; paged) → a fact (the category in the nav, ☆/★ in
// its right corner; tap, swipe left or BOOT = another one, swipe right = back, swipe up/down
// scrolls a long one; long press = play / stop playing what you're browsing). Playing: a ▶ by
// the title, tap = pause / resume, each fact stays 8 s + 70 ms a character; Dotty stays awake
// meanwhile, up to 30 min untouched. New facts come first: each category goes round all its
// facts before repeating. Locked: a fact that fits four lines, a new one every 5 minutes.

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include <vector>

#include "cartridge.h"
#include "core_ble.h"
#include "facts_store.h"
#include "images/sleep_panda.h"
#include "log.h"
#include "nav_bar.h"
#include "shell.h"
#include "storage.h"
#include "ui.h"

DOTTY_CARTRIDGE("facts", "Facts", "0.1.5");

namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

constexpr int16_t kTextX = 8;
constexpr int16_t kTextW = kW - 18;  // room for the scrollbar
constexpr int16_t kLineH = 19;
constexpr int16_t kFirstBaseline = nav::kHeight + 21;
constexpr int kVisibleLines = (EpdDisplay::kSize - 6 - kFirstBaseline) / kLineH + 1;  // 7
constexpr int kScrollLines = kVisibleLines - 2;
constexpr int16_t kRowH = 38;
constexpr int kListRows = (EpdDisplay::kSize - nav::kHeight - 2) / kRowH;  // 4
constexpr int kLockLines = 4;
// The menu's top row: Random, Play, Favourites, an icon over a label each (page 1 only).
constexpr int16_t kActionH = 56;
constexpr int kFirstPageRows = (EpdDisplay::kSize - nav::kHeight - 2 - kActionH) / kRowH;  // 2
// Playing: how long a fact stays, and when an untouched show stops (battery).
constexpr uint32_t kPlayBaseMs = 8000, kPlayPerCharMs = 70, kPlayIdleMs = 30 * 60 * 1000;

enum class Screen { Menu, Fact };
Screen screen = Screen::Menu;
bool redraw = true;
int menuPage = 0;

int scope = facts::kAny;   // what the fact screen draws from
int current = -1;          // the fact on screen
std::vector<int> history;  // facts shown in this visit, for swiping back
int historyPos = -1;
int scrollLine = 0;

bool playing = false;  // the fact screen moves on by itself
bool paused = false;
uint32_t nextAt = 0;        // millis() of the next fact while playing
uint32_t lastTouchAt = 0;   // the show stops kPlayIdleMs after this

// ---------- menu ----------

// The categories, paged: page 1 has the action row and kFirstPageRows of them, the rest kListRows.
int menuPages() {
  const int n = facts::categories().size();
  return 1 + max(0, (n - kFirstPageRows + kListRows - 1) / kListRows);
}

int firstOnPage(int page) {
  return page == 0 ? 0 : kFirstPageRows + (page - 1) * kListRows;
}

int rowsOnPage(int page) {
  return page == 0 ? kFirstPageRows : kListRows;
}

int16_t rowsTop(int page) {
  return nav::kHeight + 2 + (page == 0 ? kActionH : 0);
}

void drawPlayIcon(int16_t cx, int16_t cy, uint16_t color) {
  epd.fillTriangle(cx - 6, cy - 9, cx - 6, cy + 9, cx + 9, cy, color);
}

void drawPauseIcon(int16_t cx, int16_t cy, uint16_t color) {
  epd.fillRect(cx - 6, cy - 8, 4, 17, color);
  epd.fillRect(cx + 3, cy - 8, 4, 17, color);
}

// Random · Play · Favourites, an icon over a label each.
void drawActions() {
  const int16_t top = nav::kHeight + 2, w = kW / 3;
  const char *labels[] = {"Random", "Play", "Saved"};
  for (int i = 0; i < 3; i++) {
    const int16_t cx = i * w + w / 2, cy = top + 17;
    if (i == 0) nav::drawIcon(epd, nav::Icon::Shuffle, cx, cy, kBlack);
    if (i == 1) drawPlayIcon(cx, cy, kBlack);
    if (i == 2) nav::drawIcon(epd, nav::Icon::StarFilled, cx, cy, kBlack);
    epd.setFont(&FreeSans9pt7b);
    epd.setCursor(cx - ui::textWidth(epd, labels[i]) / 2, top + 47);
    epd.print(labels[i]);
  }
  epd.drawFastHLine(10, top + kActionH - 1, kW - 20, kBlack);
}

void drawMenu() {
  epd.fillScreen(kWhite);
  const int pages = menuPages();
  menuPage = constrain(menuPage, 0, pages - 1);
  epd.setFont(&FreeSans9pt7b);
  nav::draw(epd, "Facts", nav::Icon::None, nav::Icon::None, nav::pageLabel(menuPage, pages));
  epd.setTextColor(kBlack);
  if (menuPage == 0) drawActions();
  const std::vector<facts::Category> &cats = facts::categories();
  for (int row = 0; row < rowsOnPage(menuPage); row++) {
    const int c = firstOnPage(menuPage) + row;
    if (c >= static_cast<int>(cats.size())) break;
    const int16_t top = rowsTop(menuPage) + row * kRowH, baseline = top + kRowH / 2 + 6;
    epd.setFont(&FreeSans9pt7b);
    const String count = String(facts::scopeCount(c) - facts::seenCount(c));  // still new
    const int16_t countW = ui::textWidth(epd, count);
    epd.setCursor(10, baseline);
    epd.print(ui::fitText(epd, cats[c].name, kW - 30 - countW));
    epd.setCursor(kW - 10 - countW, baseline);
    epd.print(count);
    epd.drawFastHLine(10, top + kRowH - 1, kW - 20, kBlack);
  }
}

// ---------- a fact ----------

String scopeName() {
  if (scope == facts::kFavourites) return "Favourites";
  if (scope == facts::kAny && playing && current < 0) return "Facts";
  if (current >= 0) return facts::categories()[facts::categoryOf(current)].name;
  return "Facts";
}

std::vector<String> factLines(size_t i) {
  epd.setFont(&FreeSans9pt7b);
  return ui::wrapText(epd, ui::printable(facts::text(i)), kTextW, 30);
}

// A long fact's scrollbar on the right edge (as in News): a dotted track and a thumb.
void drawScrollbar(int first, int visible, int total) {
  if (total <= visible) return;
  const int16_t x = kW - 5, top = nav::kHeight + 6, height = EpdDisplay::kSize - 6 - top;
  for (int16_t y = top; y < top + height; y += 3) epd.drawPixel(x + 1, y, kBlack);
  const int16_t thumb = max<int16_t>(14, height * visible / total);
  const int16_t y = top + (height - thumb) * first / max(1, total - visible);
  epd.fillRoundRect(x, y, 3, thumb, 1, kBlack);
}

void drawFact() {
  epd.fillScreen(kWhite);
  epd.setFont(&FreeSans9pt7b);
  const bool star = current >= 0 && facts::isFavourite(current);
  const String title = ui::fitText(epd, scopeName(), playing ? 84 : 104);
  nav::draw(epd, title, nav::Icon::Back, star ? nav::Icon::StarFilled : nav::Icon::Star);
  if (playing) {  // ▶ (or ‖ while paused) before the title, white on the bar
    const int16_t x = (kW - ui::textWidth(epd, title)) / 2 - 14, cy = nav::kHeight / 2 + 1;
    if (paused) drawPauseIcon(x, cy, kWhite);
    else drawPlayIcon(x, cy, kWhite);
  }
  epd.setTextColor(kBlack);
  if (current < 0) {
    ui::drawWrapped(epd, scope == facts::kFavourites ? "No favourites yet. Tap the star on a fact you love."
                                                     : "No facts here.",
                    100, kW - 24, 4, 20);
    return;
  }
  const std::vector<String> lines = factLines(current);
  scrollLine = constrain(scrollLine, 0, max(0, static_cast<int>(lines.size()) - kVisibleLines));
  epd.setFont(&FreeSans9pt7b);
  // Short facts sit in the middle of the space; long ones start at the top and scroll.
  const int shown = min<int>(kVisibleLines, lines.size());
  int16_t y = kFirstBaseline;
  if (static_cast<int>(lines.size()) <= kVisibleLines) {
    const int16_t space = EpdDisplay::kSize - nav::kHeight - 8;
    y = nav::kHeight + (space - shown * kLineH) / 2 + 15;
  }
  for (int i = 0; i < shown; i++) {
    epd.setCursor(kTextX, y + i * kLineH);
    epd.print(lines[scrollLine + i]);
  }
  drawScrollbar(scrollLine, kVisibleLines, lines.size());
}

void drawApp() {
  if (screen == Screen::Menu) drawMenu();
  else drawFact();
}

// ---------- actions ----------

void notifyChanged() {
  JsonDocument event;
  event["event"] = "facts.changed";
  ble::notify(event);
}

void show(int i) {
  current = i;
  scrollLine = 0;
}

void nextFact() {
  // Forward through the history first (after swiping back), then a new one.
  if (historyPos + 1 < static_cast<int>(history.size())) {
    show(history[++historyPos]);
    return;
  }
  const int i = facts::next(scope);
  if (i < 0) return show(-1);
  history.push_back(i);
  if (history.size() > 50) history.erase(history.begin());
  historyPos = history.size() - 1;
  show(i);
}

void previousFact() {
  if (historyPos > 0) show(history[--historyPos]);
}

uint32_t showFor(int i) {
  return kPlayBaseMs + (i >= 0 ? facts::text(i).length() * kPlayPerCharMs : 0);
}

void setPlaying(bool on) {
  playing = on;
  paused = false;
  nextAt = millis() + showFor(current);
  lastTouchAt = millis();
}

void open(int s, bool play = false) {
  scope = s;
  history.clear();
  historyPos = -1;
  screen = Screen::Fact;
  nextFact();
  setPlaying(play);
}

void onGesture(Touch::Gesture gesture, uint16_t x, uint16_t y) {
  using G = Touch::Gesture;
  lastTouchAt = millis();
  if (screen == Screen::Menu) {
    const int pages = menuPages();
    if (gesture == G::SwipeLeft || gesture == G::SwipeUp) menuPage = (menuPage + 1) % pages;
    else if (gesture == G::SwipeRight || gesture == G::SwipeDown) menuPage = (menuPage + pages - 1) % pages;
    else if (gesture != G::Tap) return;
    else if (nav::hit(x, y) == 1 && pages > 1) menuPage = (menuPage + 1) % pages;
    else if (menuPage == 0 && y >= nav::kHeight && y < nav::kHeight + 2 + kActionH) {
      const int button = x / (kW / 3);
      if (button == 0) open(facts::kAny);
      else if (button == 1) open(facts::kAny, true);
      else open(facts::kFavourites);
    } else if (y >= rowsTop(menuPage)) {
      const int row = (y - rowsTop(menuPage)) / kRowH, c = firstOnPage(menuPage) + row;
      if (row >= rowsOnPage(menuPage) || c >= static_cast<int>(facts::categories().size())) return;
      open(c);
    } else {
      return;
    }
  } else {
    if (playing && (gesture == G::SwipeLeft || gesture == G::SwipeRight)) nextAt = millis() + 2 * kPlayBaseMs;
    if (gesture == G::SwipeUp) scrollLine += kScrollLines;
    else if (gesture == G::SwipeDown) scrollLine -= kScrollLines;
    else if (gesture == G::SwipeLeft) nextFact();
    else if (gesture == G::SwipeRight) previousFact();
    else if (gesture == G::LongPress && y >= nav::kHeight) setPlaying(!playing);
    else if (gesture != G::Tap) return;
    else if (nav::hit(x, y) == -1) {
      screen = Screen::Menu;
      playing = false;
    }
    else if (nav::hit(x, y) == 1) {
      if (current < 0) return;
      facts::setFavourite(current, !facts::isFavourite(current));
      notifyChanged();
    } else if (y >= nav::kHeight && playing) {
      paused = !paused;
      nextAt = millis() + showFor(current);
    } else if (y >= nav::kHeight) {
      nextFact();
    } else {
      return;
    }
  }
  redraw = true;
}

// ---------- lock screen ----------

// A fact that fits four lines, centred, on the lock widget's own canvas (not the display's font).
int16_t drawLockFact(Adafruit_GFX &gfx, const tm &now, int16_t maxHeight) {
  gfx.setFont(&FreeSans9pt7b);
  std::vector<String> lines;
  const int i = facts::lockFact(now, [&](size_t i) {
    lines = ui::wrapText(gfx, ui::printable(facts::text(i)), kW - 12, kLockLines + 1);
    return static_cast<int>(lines.size()) <= kLockLines;
  });
  if (i < 0) return 0;
  int16_t y = 14;
  for (const String &l : lines) {
    ui::drawCentered(gfx, l, y);
    y += kLineH;
  }
  return min<int16_t>(maxHeight, lines.size() * kLineH);
}

// ---------- app (BLE) ----------

void factJson(JsonObject o, size_t i) {
  o["id"] = facts::id(i);
  o["text"] = facts::text(i);
  o["category"] = facts::categories()[facts::categoryOf(i)].name;
  o["favourite"] = facts::isFavourite(i);
}

void registerCommands() {
  ble::on("facts.status", [](JsonObjectConst, JsonObject reply) {
    reply["count"] = facts::count();
    reply["seen"] = facts::seenCount(facts::kAny);
    reply["favourites"] = facts::favourites().size();
    JsonArray list = reply["categories"].to<JsonArray>();
    for (size_t c = 0; c < facts::categories().size(); c++) {
      JsonObject o = list.add<JsonObject>();
      o["key"] = facts::categories()[c].key;
      o["name"] = facts::categories()[c].name;
      o["count"] = facts::categories()[c].count;
      o["seen"] = facts::seenCount(c);
    }
    if (screen == Screen::Fact && current >= 0) factJson(reply["showing"].to<JsonObject>(), current);
  });
  // The starred facts, newest first (texts for the app's list).
  ble::on("facts.favourites", [](JsonObjectConst, JsonObject reply) {
    JsonArray list = reply["facts"].to<JsonArray>();
    const std::vector<uint16_t> &favs = facts::favourites();
    for (auto it = favs.rbegin(); it != favs.rend(); ++it) factJson(list.add<JsonObject>(), *it);
  });
  // {id, on}: star or unstar.
  ble::on("facts.favourite", [](JsonObjectConst args, JsonObject reply) {
    const int i = facts::find(args["id"] | 0u);
    if (i < 0) {
      reply["ok"] = false;
      reply["error"] = "no such fact";
      return;
    }
    facts::setFavourite(i, args["on"] | true);
    if (screen == Screen::Fact) redraw = true;
    notifyChanged();
  });
  // {category?: key}: a new fact on Dotty now (any category if none).
  ble::on("facts.next", [](JsonObjectConst args, JsonObject reply) {
    const String key = args["category"] | "";
    int s = facts::kAny;
    for (size_t c = 0; c < facts::categories().size(); c++) {
      if (key == facts::categories()[c].key) s = c;
    }
    if (screen != Screen::Fact || s != scope) open(s, playing);
    else nextFact();
    redraw = true;
    if (current >= 0) factJson(reply["showing"].to<JsonObject>(), current);
    notifyChanged();
  });
}

const shell::Picture kOffPictures[] = {
    {kSleepPanda, kSleepPandaWidth, kSleepPandaHeight},
};

}  // namespace

void setup() {
  shell::Config config;
  config.drawApp = drawApp;
  config.drawLockWidget = drawLockFact;
  config.offPictures = kOffPictures;
  config.offPictureCount = sizeof(kOffPictures) / sizeof(kOffPictures[0]);
  shell::begin(config);

  storage::begin();
  facts::begin();
  registerCommands();
  shell::showApp();
}

void loop() {
  shell::Input input;
  if (!shell::update(input)) return;
  if (input.gesture != Touch::Gesture::None) onGesture(input.gesture, shell::touch.x(), shell::touch.y());
  if (input.boot && screen == Screen::Fact) {
    lastTouchAt = millis();
    nextFact();
    if (playing) nextAt = millis() + showFor(current);
    redraw = true;
  }
  // Playing: the next fact when its time is up; awake meanwhile, until 30 min untouched.
  if (playing && screen == Screen::Fact) {
    if (millis() - lastTouchAt > kPlayIdleMs) {
      playing = false;
      redraw = true;
    } else {
      shell::wake();
      if (!paused && static_cast<int32_t>(millis() - nextAt) >= 0 && !epd.isBusy()) {
        nextFact();
        nextAt = millis() + showFor(current);
        redraw = true;
      }
    }
  }
  if (input.key == 'P' || input.key == 'R') {  // developer aid: Play / Random from anywhere
    open(facts::kAny, input.key == 'P');
    redraw = true;
  }
  if (input.key == 'n' && screen == Screen::Fact) {  // developer aid: the next fact
    nextFact();
    redraw = true;
  }
  if (redraw && !epd.isBusy()) {
    redraw = false;
    drawApp();
    shell::refresh(false);
  }
  delay(10);
}
