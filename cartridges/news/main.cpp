// News: short, recent stories for the topics the user follows (news_store.h): outlets' feeds,
// Kagi News daily briefings and Google News keywords, fetched every 30 minutes and kept on
// the SD card so they can be read offline.
//
// Screens: the menu (★ Favourites = the starred topics together, then each topic with its
// story count; paged; the top-left corner = sync now, with a progress screen) → a story (headline, source and age, the one-sentence summary; swipe
// up/down scrolls, swipe left/right or BOOT = next/previous story; the "3/10" corner = next).
// While locked, the lock screen shows a favourite headline that changes every 5 minutes.
// Topics are managed in the app (news.topic.*).

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include <vector>

#include "cartridge.h"
#include "core_ble.h"
#include "images/sleep_panda.h"
#include "log.h"
#include "nav_bar.h"
#include "net.h"
#include "news_store.h"
#include "power.h"
#include "shell.h"
#include "storage.h"
#include "ui.h"

DOTTY_CARTRIDGE("news", "News", "0.2.6");

namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

// Text area under the nav bar (as in Joke Factory).
constexpr int16_t kTextX = 8;
constexpr int16_t kTextW = kW - 16;
constexpr int16_t kLineH = 19;
constexpr int16_t kFirstBaseline = nav::kHeight + 19;
constexpr int kVisibleLines = (EpdDisplay::kSize - 4 - kFirstBaseline) / kLineH + 1;  // 7
constexpr int kScrollLines = kVisibleLines - 2;  // a swipe moves this many lines
constexpr int16_t kRowH = 38;
constexpr int kListRows = (EpdDisplay::kSize - nav::kHeight - 2) / kRowH;  // 4
constexpr int kLockHeadlineLines = 3;  // + the source line = 4

enum class Screen { Menu, Story, Message, Sync };
Screen screen = Screen::Menu;
bool sdReady = false;
bool redraw = true;
int menuPage = 0;

// The story list on screen: a topic's key ("" = favourites) and where we are in it. The
// pointers are re-made whenever the pool reloads.
String listKey;
String listName;
std::vector<const news::Story *> list;
int storyIndex = 0;
String shownTitle;  // to find the same story again after a reload
int scrollLine = 0;

// The sync screen: progress while the fetch runs, then how it went for a few seconds.
constexpr uint32_t kSyncOkMs = 2000, kSyncProblemMs = 8000;
String syncNote;          // why it couldn't start (no Wi-Fi, no topics), else ""
bool syncDone = false;    // the fetch finished while the screen was open
String syncError;         // that fetch's problem ("" = every topic came back)
uint32_t syncDoneAt = 0;
int syncShownDone = -1;   // the progress on screen (redraw when it moves)

// ---------- menu rows ----------

struct MenuRow {
  String key, name;  // key "" = favourites
};

std::vector<MenuRow> menuRows() {
  std::vector<MenuRow> rows = {{"", "Favourites"}};
  for (const news::Topic &t : news::topics()) rows.push_back({t.key(), t.name});
  return rows;
}

int menuPages() {
  return max(1, (static_cast<int>(menuRows().size()) + kListRows - 1) / kListRows);
}

// ---------- drawing ----------

struct Line {
  String text;
  bool bold = false;
  bool rule = false;  // a divider under it (the source line, before the summary)
};

std::vector<Line> storyLines(const news::Story &s) {
  std::vector<Line> lines;
  epd.setFont(&FreeSansBold9pt7b);
  for (const String &l : ui::wrapText(epd, ui::printable(s.title), kTextW, 30)) lines.push_back({l, true});
  epd.setFont(&FreeSans9pt7b);
  lines.push_back({ui::fitText(epd, ui::printable(s.source) + " - " + news::age(s), kTextW), false, s.summary[0] != 0});
  if (s.summary[0]) {
    for (const String &l : ui::wrapText(epd, ui::printable(s.summary), kTextW, 30)) lines.push_back({l});
  }
  return lines;
}

// Two arrows chasing each other round a circle: "sync now" (the menu's top-left corner).
void drawSyncIcon(int16_t cx, int16_t cy, uint16_t color) {
  auto at = [&](float degrees, float r, int16_t &x, int16_t &y) {
    x = cx + lroundf(r * cosf(degrees * PI / 180));
    y = cy + lroundf(r * sinf(degrees * PI / 180));
  };
  int16_t x, y, x1, y1, x2, y2;
  for (int start : {200, 20}) {  // the top arc, then the bottom one, both clockwise
    const float end = start + 120;
    for (float d = start; d <= end; d += 2) {
      for (float r = 7.5f; r <= 9.5f; r += 1) {
        at(d, r, x, y);
        epd.drawPixel(x, y, color);
      }
    }
    at(end + 30, 8.5f, x, y);  // the arrow's tip, further round the circle
    at(end, 3.5f, x1, y1);
    at(end, 13.5f, x2, y2);
    epd.fillTriangle(x, y, x1, y1, x2, y2, color);
  }
}

void drawMenu() {
  epd.fillScreen(kWhite);
  const std::vector<MenuRow> rows = menuRows();
  const int pages = menuPages();
  menuPage = constrain(menuPage, 0, pages - 1);
  nav::draw(epd, "News", nav::Icon::None, nav::Icon::None, nav::pageLabel(menuPage, pages));
  drawSyncIcon(nav::kButton / 2, nav::kHeight / 2, kWhite);
  epd.setFont(&FreeSans9pt7b);
  for (int row = 0; row < kListRows; row++) {
    const int i = menuPage * kListRows + row;
    if (i >= static_cast<int>(rows.size())) break;
    const int16_t top = nav::kHeight + 2 + row * kRowH;
    const String count = String(news::stories(rows[i].key).size());
    const int16_t countW = ui::textWidth(epd, count);
    int16_t x = 10;
    if (rows[i].key.isEmpty()) {  // favourites: a star first
      nav::drawIcon(epd, nav::Icon::StarFilled, 20, top + kRowH / 2 - 1, kBlack);
      x = 36;
    }
    epd.setCursor(x, top + kRowH / 2 + 6);
    epd.print(ui::fitText(epd, ui::printable(rows[i].name), kW - x - 20 - countW));
    epd.setCursor(kW - 10 - countW, top + kRowH / 2 + 6);
    epd.print(count);
    epd.drawFastHLine(10, top + kRowH - 1, kW - 20, kBlack);
  }
}

// A long story's scrollbar on the right edge: a thin track, and a thumb as tall as the part on
// screen, placed where it is in the story. Nothing when it all fits.
void drawScrollbar(int first, int visible, int total) {
  if (total <= visible) return;
  const int16_t x = kW - 4, top = nav::kHeight + 4, height = EpdDisplay::kSize - 4 - top;
  for (int16_t y = top; y < top + height; y += 3) epd.drawPixel(x + 1, y, kBlack);  // dotted track
  const int16_t thumb = max<int16_t>(14, height * visible / total);
  const int16_t y = top + (height - thumb) * first / max(1, total - visible);
  epd.fillRoundRect(x, y, 3, thumb, 1, kBlack);
}

void drawStory() {
  epd.fillScreen(kWhite);
  const String where = list.empty() ? String() : String(storyIndex + 1) + "/" + String(list.size());
  epd.setFont(&FreeSans9pt7b);
  nav::draw(epd, ui::fitText(epd, ui::printable(listName), 96), nav::Icon::Back, nav::Icon::None, where);
  if (list.empty()) {
    epd.setFont(&FreeSans9pt7b);
    ui::drawWrapped(epd, "No stories here yet. They come every 30 minutes over Wi-Fi.", 90, kW - 24, 4, 20);
    return;
  }
  const std::vector<Line> lines = storyLines(*list[storyIndex]);
  scrollLine = constrain(scrollLine, 0, max(0, static_cast<int>(lines.size()) - kVisibleLines));
  for (int i = 0; i < kVisibleLines && scrollLine + i < static_cast<int>(lines.size()); i++) {
    const Line &line = lines[scrollLine + i];
    epd.setFont(line.bold ? &FreeSansBold9pt7b : &FreeSans9pt7b);
    epd.setCursor(kTextX, kFirstBaseline + i * kLineH);
    epd.print(line.text);
    if (line.rule) epd.drawFastHLine(kTextX, kFirstBaseline + i * kLineH + 5, kTextW, kBlack);
  }
  drawScrollbar(scrollLine, kVisibleLines, lines.size());
}

// No stories at all yet: why, and what happens next.
void drawMessage() {
  epd.fillScreen(kWhite);
  nav::draw(epd, "News", nav::Icon::None, nav::Icon::None);
  const news::FetchState fetch = news::fetchState();
  epd.setFont(&FreeSansBold9pt7b);
  if (!sdReady) {
    ui::drawCentered(epd, "No SD card", 105);
    return;
  }
  if (fetch.running) {
    ui::drawCentered(epd, "Getting the news", 90);
    epd.setFont(&FreeSans9pt7b);
    ui::drawCentered(epd, String(fetch.done) + " of " + String(fetch.total) + " topics", 118);
    return;
  }
  ui::drawCentered(epd, "No news yet", 80);
  epd.setFont(&FreeSans9pt7b);
  if (net::saved().empty()) {
    ui::drawWrapped(epd, "Dotty needs Wi-Fi to get the news. Add a network in the app.", 112, kW - 24, 3, 20);
  } else {
    ui::drawCentered(epd, "No internet right now", 112);
    ui::drawCentered(epd, ui::fitText(epd, fetch.error, kW - 16), 132);
    ui::drawCentered(epd, "Dotty will try again", 160);
  }
}

// "12 min ago" for the last good fetch (local time, like Dotty's clock).
String fetchedAgo(uint32_t fetchedAt) {
  const uint32_t now = time(nullptr);
  if (!fetchedAt || now < fetchedAt) return "";
  const uint32_t minutes = (now - fetchedAt) / 60;
  if (minutes < 1) return "just now";
  if (minutes < 60) return String(minutes) + " min ago";
  if (minutes < 48 * 60) return String(minutes / 60) + " h ago";
  return String(minutes / 1440) + " days ago";
}

void drawSync() {
  epd.fillScreen(kWhite);
  nav::draw(epd, "Sync", nav::Icon::Back, nav::Icon::None);
  const news::FetchState fetch = news::fetchState();
  if (syncNote.length()) {
    epd.setFont(&FreeSansBold9pt7b);
    ui::drawCentered(epd, "Can't sync", 85);
    epd.setFont(&FreeSans9pt7b);
    ui::drawWrapped(epd, syncNote, 115, kW - 24, 3, 20);
    return;
  }
  if (!syncDone) {
    epd.setFont(&FreeSansBold9pt7b);
    ui::drawCentered(epd, "Getting the news", 88);
    // A bar by topics done.
    constexpr int16_t kBarX = 20, kBarY = 104, kBarW = kW - 40, kBarH = 12;
    epd.drawRoundRect(kBarX, kBarY, kBarW, kBarH, 3, kBlack);
    const int16_t filled = fetch.total ? (kBarW - 4) * fetch.done / fetch.total : 0;
    if (filled > 0) epd.fillRect(kBarX + 2, kBarY + 2, filled, kBarH - 4, kBlack);
    epd.setFont(&FreeSans9pt7b);
    // Joining Wi-Fi takes the first ~10 s, before any topic.
    ui::drawCentered(epd, fetch.done ? String(fetch.done) + " of " + String(fetch.total) + " topics" : String("Joining Wi-Fi..."), 140);
    if (fetch.fetchedAt) ui::drawCentered(epd, "Last time: " + fetchedAgo(fetch.fetchedAt), 166);
    return;
  }
  epd.setFont(&FreeSansBold9pt7b);
  if (syncError.isEmpty()) {
    ui::drawCentered(epd, "Up to date", 95);
    epd.setFont(&FreeSans9pt7b);
    ui::drawCentered(epd, String(news::storyCount()) + " stories", 125);
    return;
  }
  ui::drawCentered(epd, "Some topics failed", 80);
  epd.setFont(&FreeSans9pt7b);
  ui::drawWrapped(epd, ui::printable(syncError), 108, kW - 24, 3, 20);
  ui::drawCentered(epd, "The others are up to date", 180);
}

void drawApp() {
  if (screen == Screen::Message && news::storyCount()) screen = Screen::Menu;
  switch (screen) {
    case Screen::Menu: drawMenu(); break;
    case Screen::Story: drawStory(); break;
    case Screen::Message: drawMessage(); break;
    case Screen::Sync: drawSync(); break;
  }
}

// ---------- actions ----------

void notifyChanged() {
  JsonDocument event;
  event["event"] = "news.changed";
  ble::notify(event);
}

void showStory(int index) {
  if (list.empty()) {
    storyIndex = 0;
    shownTitle = "";
  } else {
    storyIndex = (index % static_cast<int>(list.size()) + list.size()) % list.size();
    shownTitle = list[storyIndex]->title;
  }
  scrollLine = 0;
  screen = Screen::Story;
}

// The menu's sync corner: fetch every topic now and watch it happen. Leaving the screen
// doesn't stop the fetch.
void openSync() {
  syncNote = "";
  syncDone = false;
  syncError = "";
  syncShownDone = -1;
  if (net::saved().empty()) syncNote = "Dotty needs Wi-Fi to get the news. Add a network in the app.";
  else if (news::topics().empty()) syncNote = "No topics yet. Add some in the app.";
  else news::startFetch();
  syncDoneAt = syncNote.length() ? millis() : 0;
  screen = Screen::Sync;
}

void openList(const MenuRow &row) {
  listKey = row.key;
  listName = row.name;
  list = news::stories(listKey);
  showStory(0);
}

// The pool reloaded (or topics changed): new pointers, the same story if it's still there.
void refreshList() {
  if (screen != Screen::Story) return;
  list = news::stories(listKey);
  for (size_t i = 0; i < list.size(); i++) {
    if (shownTitle == list[i]->title) {
      storyIndex = i;
      return;
    }
  }
  storyIndex = constrain(storyIndex, 0, max(0, static_cast<int>(list.size()) - 1));
  scrollLine = 0;
}

void onGesture(Touch::Gesture gesture, uint16_t x, uint16_t y) {
  using G = Touch::Gesture;
  switch (screen) {
    case Screen::Menu: {
      const int pages = menuPages();
      if (gesture == G::SwipeLeft || gesture == G::SwipeUp) menuPage = (menuPage + 1) % pages;
      else if (gesture == G::SwipeRight || gesture == G::SwipeDown) menuPage = (menuPage + pages - 1) % pages;
      else if (gesture != G::Tap) return;
      else if (nav::hit(x, y) == -1) openSync();
      else if (nav::hit(x, y) == 1 && pages > 1) menuPage = (menuPage + 1) % pages;
      else if (y >= nav::kHeight + 2) {
        const std::vector<MenuRow> rows = menuRows();
        const int row = (y - nav::kHeight - 2) / kRowH;
        const int i = menuPage * kListRows + row;
        if (row >= kListRows || i >= static_cast<int>(rows.size())) return;
        openList(rows[i]);
      } else {
        return;
      }
      break;
    }
    case Screen::Story:
      if (gesture == G::SwipeUp) scrollLine += kScrollLines;
      else if (gesture == G::SwipeDown) scrollLine -= kScrollLines;
      else if (gesture == G::SwipeLeft) showStory(storyIndex + 1);
      else if (gesture == G::SwipeRight) showStory(storyIndex - 1);
      else if (gesture != G::Tap) return;
      else if (nav::hit(x, y) == -1) screen = Screen::Menu;
      else if (nav::hit(x, y) == 1) showStory(storyIndex + 1);
      else return;
      break;
    case Screen::Sync:
      // Back (or, once it's done, a tap anywhere) returns to the menu; the fetch carries on.
      if (gesture != G::Tap || (nav::hit(x, y) != -1 && !syncDone && syncNote.isEmpty())) return;
      screen = Screen::Menu;
      break;
    case Screen::Message:
      return;
  }
  redraw = true;
}

// ---------- lock screen ----------

// A favourite headline (bold, up to 3 lines) and its source, centred. Measured on `gfx` (the
// lock widget's own canvas), so the display's font is left alone.
int16_t drawLockStory(Adafruit_GFX &gfx, const tm &now, int16_t maxHeight) {
  const news::Story *s = news::lockStory(now);
  if (!s) return 0;
  gfx.setFont(&FreeSansBold9pt7b);
  const std::vector<String> lines = ui::wrapText(gfx, ui::printable(s->title), kTextW, kLockHeadlineLines);
  int16_t y = 14;
  for (const String &l : lines) {
    ui::drawCentered(gfx, l, y);
    y += kLineH;
  }
  gfx.setFont(&FreeSans9pt7b);
  ui::drawCentered(gfx, ui::fitText(gfx, ui::printable(s->source) + " - " + news::age(*s), kTextW), y);
  return min<int16_t>(maxHeight, (lines.size() + 1) * kLineH - 1);
}

void measureLockFit() {
  // 3 headline lines at most, nothing cut off (asking for 4 shows when it needs more).
  news::measureLockFit([](const news::Story &s) {
    epd.setFont(&FreeSansBold9pt7b);
    return ui::wrapText(epd, ui::printable(s.title), kTextW, kLockHeadlineLines + 1).size() <= kLockHeadlineLines;
  });
}

// ---------- app (BLE) ----------

void fail(JsonObject reply, const String &error) {
  reply["ok"] = false;
  reply["error"] = error;
}

void topicsChanged() {
  refreshList();
  if (screen == Screen::Menu || screen == Screen::Story) redraw = true;
  notifyChanged();
}

void registerCommands() {
  ble::on("news.status", [](JsonObjectConst, JsonObject reply) {
    reply["stories"] = news::storyCount();
    const news::FetchState fetch = news::fetchState();
    reply["fetching"] = fetch.running;
    reply["done"] = fetch.done;
    reply["total"] = fetch.total;
    reply["fetchedAt"] = fetch.fetchedAt;  // local time, epoch seconds
    if (fetch.error.length()) reply["error"] = fetch.error;
    JsonArray list = reply["topics"].to<JsonArray>();
    for (const news::Topic &t : news::topics()) {
      JsonObject o = list.add<JsonObject>();
      o["key"] = t.key();
      o["name"] = t.name;
      if (t.section.length()) o["section"] = t.section;
      else if (t.kagi.length()) o["kagi"] = t.kagi;
      else o["query"] = t.query;
      o["star"] = t.star;
      o["count"] = news::stories(t.key()).size();
    }
  });
  // The outlets' feeds a topic can be (the app groups them by outlet).
  ble::on("news.sections", [](JsonObjectConst, JsonObject reply) {
    JsonArray list = reply["sections"].to<JsonArray>();
    for (const news::Feed &f : news::feeds()) {
      JsonObject o = list.add<JsonObject>();
      o["id"] = f.id;
      o["outlet"] = f.outlet;
      o["section"] = f.section;
      o["name"] = f.name();
    }
  });
  // {section} (an outlet feed), {kagi: "formula_1.json", name} (a Kagi News category, from
  // kite.kagi.com/kite.json), or {query, name?} (a Google News keyword).
  ble::on("news.topic.add", [](JsonObjectConst args, JsonObject reply) {
    news::Topic t;
    t.section = args["section"] | "";
    t.kagi = args["kagi"] | "";
    t.query = args["query"] | "";
    t.query.trim();
    if (t.section.length()) {
      for (const news::Feed &f : news::feeds()) {
        if (t.section == f.id) t.name = f.name();
      }
      if (t.name.isEmpty()) return fail(reply, "unknown feed");
    } else if (t.kagi.length()) {
      // A plain file name only (it becomes part of a URL).
      if (!t.kagi.endsWith(".json") || t.kagi.indexOf('/') >= 0 || t.kagi.length() > 60) return fail(reply, "bad category");
      t.name = args["name"] | "";
      if (t.name.isEmpty()) t.name = t.kagi.substring(0, t.kagi.length() - 5);
    } else if (t.query.length()) {
      if (t.query.length() > 60) return fail(reply, "keep it under 60 characters");
      t.name = args["name"] | t.query.c_str();
    } else {
      return fail(reply, "section or query needed");
    }
    t.star = args["star"] | false;
    String error;
    if (!news::addTopic(t, error)) return fail(reply, error);
    reply["key"] = t.key();
    topicsChanged();
  });
  ble::on("news.topic.remove", [](JsonObjectConst args, JsonObject reply) {
    if (!news::removeTopic(args["key"] | "")) return fail(reply, "no such topic");
    if (screen == Screen::Story && listKey.length() && listKey == String(args["key"] | "")) screen = Screen::Menu;
    topicsChanged();
  });
  ble::on("news.topic.star", [](JsonObjectConst args, JsonObject reply) {
    if (!news::starTopic(args["key"] | "", args["on"] | true)) return fail(reply, "no such topic");
    topicsChanged();
  });
  ble::on("news.topic.move", [](JsonObjectConst args, JsonObject reply) {
    if (!news::moveTopic(args["from"] | -1, args["to"] | -1)) return fail(reply, "bad position");
    topicsChanged();
  });
  ble::on("news.refresh", [](JsonObjectConst, JsonObject reply) {
    if (net::saved().empty()) return fail(reply, "Dotty has no Wi-Fi yet");
    news::startFetch();
  });
  // {topic? (key; "" = favourites), limit? = 15}: what Dotty shows, best first.
  ble::on("news.stories", [](JsonObjectConst args, JsonObject reply) {
    const int limit = constrain(args["limit"] | 15, 1, 40);
    JsonArray out = reply["stories"].to<JsonArray>();
    int n = 0;
    for (const news::Story *s : news::stories(args["topic"] | "")) {
      if (n++ >= limit) break;
      JsonObject o = out.add<JsonObject>();
      o["title"] = s->title;
      o["source"] = s->source;
      o["summary"] = s->summary;
      o["published"] = s->published;  // UTC epoch
      o["topic"] = s->topic;
    }
  });
}

const shell::Picture kOffPictures[] = {
    {kSleepPanda, kSleepPandaWidth, kSleepPandaHeight},
};

}  // namespace

void setup() {
  shell::Config config;
  config.drawApp = drawApp;
  config.drawLockWidget = drawLockStory;
  config.offPictures = kOffPictures;
  config.offPictureCount = sizeof(kOffPictures) / sizeof(kOffPictures[0]);
  shell::begin(config);

  sdReady = storage::begin();
  if (sdReady) {
    news::begin();
    measureLockFit();
  }
  if (!news::storyCount()) screen = Screen::Message;
  registerCommands();
  shell::showApp();
}

void loop() {
  power::setWakeLock(power::kWakeLockNetwork, news::fetchState().running);

  shell::Input input;
  const bool unlocked = shell::update(input);

  // Every 30 minutes, also while locked (the shell wakes once a minute).
  if (sdReady && news::fetchDue()) {
    news::startFetch();
    if (screen == Screen::Message) redraw = true;
  }
  if (news::takeFetchFinished()) {
    measureLockFit();
    refreshList();
    notifyChanged();
    if (screen == Screen::Sync && syncNote.isEmpty()) {
      syncDone = true;
      syncError = news::fetchState().error;
      syncDoneAt = millis();
    }
    redraw = true;
  }
  if (!unlocked) return;

  if (input.gesture != Touch::Gesture::None) onGesture(input.gesture, shell::touch.x(), shell::touch.y());
  if (input.boot && screen == Screen::Story) {
    showStory(storyIndex + 1);
    redraw = true;
  }
  if (input.boot && screen == Screen::Sync) {
    screen = Screen::Menu;
    redraw = true;
  }
  if (input.key == 'r') {  // developer aid: the sync corner
    openSync();
    redraw = true;
  }

  // The sync screen: follow the progress, then back to the menu after a moment.
  if (screen == Screen::Sync) {
    if (!syncDone && syncNote.isEmpty() && news::fetchState().done != syncShownDone) {
      syncShownDone = news::fetchState().done;
      redraw = true;
    }
    const uint32_t shownFor = syncNote.length() || syncError.length() ? kSyncProblemMs : kSyncOkMs;
    if ((syncDone || syncNote.length()) && millis() - syncDoneAt > shownFor && syncDoneAt) {
      screen = Screen::Menu;
      syncDoneAt = 0;
      redraw = true;
    }
  }

  // Fetch progress on the "no news yet" screen.
  static uint32_t lastProgress = 0;
  if (screen == Screen::Message && news::fetchState().running && millis() - lastProgress > 3000) {
    lastProgress = millis();
    redraw = true;
  }

  if (redraw && !epd.isBusy()) {
    redraw = false;
    drawApp();
    shell::refresh(false);
  }
  delay(10);
}
