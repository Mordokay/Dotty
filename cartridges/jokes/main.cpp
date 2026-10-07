// Joke Factory: jokes from JokeAPI, all of them kept on the SD card (joke_store.h).
//
// Screens: a 2x2 grid of categories (Dark, Programming, Misc, Any; star top-right =
// favourites) → a joke (two-part jokes show the punchline on a tap or BOOT; swipe up/down
// scrolls long ones; the star keeps it) → favourites (a paged list). BOOT on a joke whose
// punchline is showing fetches the next one. While locked, the lock screen shows a short
// joke that changes every 5 minutes on the clock.

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include <vector>

#include "cartridge.h"
#include "core_ble.h"
#include "images/sleep_panda.h"
#include "images/sleep_portrait.h"
#include "joke_store.h"
#include "log.h"
#include "nav_bar.h"
#include "net.h"
#include "power.h"
#include "shell.h"
#include "storage.h"
#include "ui.h"

DOTTY_CARTRIDGE("jokes", "Joke Factory", "0.2.7");

namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

// Text area under the nav bar.
constexpr int16_t kTextX = 8;
constexpr int16_t kTextW = kW - 16;
constexpr int16_t kLineH = 19;
constexpr int16_t kFirstBaseline = nav::kHeight + 19;
constexpr int kVisibleLines = (EpdDisplay::kSize - 4 - kFirstBaseline) / kLineH + 1;  // 7
constexpr int kScrollLines = kVisibleLines - 2;  // a swipe moves this many lines (e-paper can't follow a finger)
constexpr int16_t kRowH = 38;    // favourites list
constexpr int kListRows = (EpdDisplay::kSize - nav::kHeight - 2) / kRowH;

// The grid, in reading order.
const int kGrid[4] = {jokes::Dark, jokes::Programming, jokes::Misc, jokes::kAny};

enum class Screen { Grid, Joke, Favourites, Message };
Screen screen = Screen::Grid;
bool sdReady = false;

// The joke on screen (a copy: the store may reload under it).
struct Shown {
  uint16_t id = 0;
  uint8_t category = 0;
  String setup, punchline;
  bool fromFavourites = false;
  int favouriteIndex = 0;
};
Shown shown;
int shownFrom = jokes::kAny;  // the grid cell it came from (BOOT = another from there)
bool revealed = false;
int scrollLine = 0;
int listPage = 0;
int waitingCategory = jokes::kAny;  // the cell tapped while jokes were downloading
bool redraw = true;

// The name shown for a category: JokeAPI's own, except "Programming" (too wide) → "Code".
// Labels stay at 6 characters or fewer.
String label(int category) {
  return category == jokes::Programming ? String("Code") : String(jokes::categoryName(category));
}

// ---------- drawing helpers ----------

struct Line {
  String text;
  bool bold = false;
  bool hint = false;  // centred, e.g. "tap for the punchline"
};

std::vector<Line> jokeLines() {
  std::vector<Line> lines;
  epd.setFont(&FreeSans9pt7b);
  for (const String &l : ui::wrapText(epd, shown.setup, kTextW, 60)) lines.push_back({l});
  if (shown.punchline.isEmpty()) return lines;
  lines.push_back({""});
  if (!revealed) {
    lines.push_back({"tap for the punchline", false, true});
    return lines;
  }
  epd.setFont(&FreeSansBold9pt7b);
  for (const String &l : ui::wrapText(epd, shown.punchline, kTextW, 60)) lines.push_back({l, true});
  return lines;
}

void thickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  for (int d = 0; d < 3; d++) {
    epd.drawLine(x0, y0 + d, x1, y1 + d, kBlack);
    epd.drawLine(x0 + d, y0, x1 + d, y1, kBlack);
  }
}

void drawCategoryIcon(int category, int16_t cx, int16_t cy) {
  switch (category) {
    case jokes::Dark:  // a crescent moon
      epd.fillCircle(cx, cy, 14, kBlack);
      epd.fillCircle(cx + 8, cy - 6, 12, kWhite);
      break;
    case jokes::Programming:  // </>
      thickLine(cx - 8, cy - 9, cx - 17, cy);
      thickLine(cx - 17, cy, cx - 8, cy + 9);
      thickLine(cx + 7, cy - 9, cx + 16, cy);
      thickLine(cx + 16, cy, cx + 7, cy + 9);
      thickLine(cx + 2, cy - 12, cx - 4, cy + 11);
      break;
    case jokes::Misc:  // a die
      epd.drawRoundRect(cx - 13, cy - 13, 27, 27, 5, kBlack);
      epd.drawRoundRect(cx - 12, cy - 12, 25, 25, 4, kBlack);
      epd.fillCircle(cx - 6, cy - 6, 3, kBlack);
      epd.fillCircle(cx, cy, 3, kBlack);
      epd.fillCircle(cx + 6, cy + 6, 3, kBlack);
      break;
    default:  // Any: shuffle arrows
      nav::drawIcon(epd, nav::Icon::Shuffle, cx, cy, kBlack);
      break;
  }
}

void cellRect(int i, int16_t &x, int16_t &y, int16_t &w, int16_t &h) {
  w = 96;
  h = 72;
  x = 2 + (i % 2) * 100;
  y = nav::kHeight + 4 + (i / 2) * (h + 4);
}

void drawGrid() {
  epd.fillScreen(kWhite);
  nav::draw(epd, "Jokes", nav::Icon::None, nav::Icon::StarFilled);
  for (int i = 0; i < 4; i++) {
    int16_t x, y, w, h;
    cellRect(i, x, y, w, h);
    epd.drawRoundRect(x, y, w, h, 8, kBlack);
    epd.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 7, kBlack);
    drawCategoryIcon(kGrid[i], x + w / 2, y + 27);
    epd.setFont(&FreeSansBold9pt7b);
    const String name = label(kGrid[i]);
    epd.setCursor(x + (w - ui::textWidth(epd, name)) / 2, y + 61);
    epd.print(name);
  }
}

void drawJoke() {
  epd.fillScreen(kWhite);
  // From favourites: where in the list ("14/117"); otherwise the category.
  const String title = shown.fromFavourites
                           ? String(shown.favouriteIndex + 1) + "/" + String(jokes::favourites().size())
                           : label(shown.category);
  nav::draw(epd, title, nav::Icon::Back,
            jokes::isFavourite(shown.id) ? nav::Icon::StarFilled : nav::Icon::Star);
  const std::vector<Line> lines = jokeLines();
  scrollLine = constrain(scrollLine, 0, max(0, static_cast<int>(lines.size()) - kVisibleLines));
  for (int i = 0; i < kVisibleLines && scrollLine + i < static_cast<int>(lines.size()); i++) {
    const Line &line = lines[scrollLine + i];
    const int16_t y = kFirstBaseline + i * kLineH;
    epd.setFont(line.bold ? &FreeSansBold9pt7b : &FreeSans9pt7b);
    if (line.hint) {
      const int16_t w = ui::textWidth(epd, line.text) + 16;
      epd.drawRoundRect((kW - w) / 2, y - 14, w, 19, 9, kBlack);
      ui::drawCentered(epd, line.text, y);
    } else {
      epd.setCursor(kTextX, y);
      epd.print(line.text);
    }
  }
  // More above / below.
  if (scrollLine > 0) epd.fillTriangle(kW - 9, nav::kHeight + 3, kW - 14, nav::kHeight + 9, kW - 4, nav::kHeight + 9, kBlack);
  if (scrollLine + kVisibleLines < static_cast<int>(lines.size())) {
    epd.fillTriangle(kW - 9, kW - 3, kW - 14, kW - 9, kW - 4, kW - 9, kBlack);
  }
}

void drawFavourites() {
  epd.fillScreen(kWhite);
  const auto &favs = jokes::favourites();
  // Every row holds a joke; the page ("1/2") sits in the nav bar's right corner.
  const int perPage = kListRows;
  const int pages = max<int>(1, (favs.size() + perPage - 1) / perPage);
  listPage = constrain(listPage, 0, pages - 1);
  nav::draw(epd, "Favourites", nav::Icon::Back, nav::Icon::None, nav::pageLabel(listPage, pages));
  epd.setFont(&FreeSans9pt7b);
  if (favs.empty()) {
    ui::drawCentered(epd, "No favourites yet", 90);
    ui::drawCentered(epd, "Tap the star on a joke", 120);
    ui::drawCentered(epd, "to keep it here", 140);
    return;
  }
  for (int row = 0; row < perPage; row++) {
    const int i = listPage * perPage + row;
    if (i >= static_cast<int>(favs.size())) break;
    const int16_t top = nav::kHeight + 2 + row * kRowH;
    epd.setCursor(10, top + kRowH / 2 + 6);
    epd.print(ui::fitText(epd, String(i + 1) + ". " + favs[i].setup, kW - 20));
    epd.drawFastHLine(10, top + kRowH - 1, kW - 20, kBlack);
  }
}

// No jokes on the card yet: why, and what happens next.
void drawMessage() {
  epd.fillScreen(kWhite);
  nav::draw(epd, label(waitingCategory), nav::Icon::Back, nav::Icon::None);
  const jokes::SyncState sync = jokes::syncState();
  epd.setFont(&FreeSansBold9pt7b);
  if (!sdReady) {
    ui::drawCentered(epd, "No SD card", 105);
    return;
  }
  if (sync.running) {
    ui::drawCentered(epd, "Getting the jokes", 90);
    epd.setFont(&FreeSans9pt7b);
    const int pct = sync.total ? sync.done * 100 / sync.total : 0;
    ui::drawCentered(epd, String(pct) + "%", 118);
    ui::drawCentered(epd, "once, over Wi-Fi", 146);
    return;
  }
  ui::drawCentered(epd, "No jokes yet", 80);
  epd.setFont(&FreeSans9pt7b);
  if (net::saved().empty()) {
    ui::drawCentered(epd, "Dotty needs Wi-Fi once", 112);
    ui::drawCentered(epd, "to get them. Add a", 132);
    ui::drawCentered(epd, "network in the app.", 152);
  } else {
    ui::drawCentered(epd, "No internet right now", 112);
    ui::drawCentered(epd, ui::fitText(epd, sync.error, kW - 16), 132);
    ui::drawCentered(epd, "Dotty will try again", 160);
  }
}

void drawApp() {
  switch (screen) {
    case Screen::Grid: drawGrid(); break;
    case Screen::Joke: drawJoke(); break;
    case Screen::Favourites: drawFavourites(); break;
    case Screen::Message: drawMessage(); break;
  }
}

// ---------- actions ----------

void notifyChanged() {
  JsonDocument event;
  event["event"] = "jokes.changed";
  ble::notify(event);
}

// The next joke from a grid cell, or the "no jokes yet" screen.
void openJoke(int category) {
  const jokes::Joke *j = jokes::pick(category);
  if (!j) {
    waitingCategory = category;
    screen = Screen::Message;
    return;
  }
  shown = {j->id, j->category, j->setup, j->punchline, false, 0};
  shownFrom = category;
  revealed = false;
  scrollLine = 0;
  screen = Screen::Joke;
  jokes::markSeen(j->id);
}

void openFavourite(int index) {
  const auto &favs = jokes::favourites();
  if (favs.empty()) {
    screen = Screen::Favourites;
    return;
  }
  index = (index % static_cast<int>(favs.size()) + favs.size()) % favs.size();
  const jokes::Favourite &f = favs[index];
  shown = {f.id, f.category, f.setup, f.punchline, true, index};
  revealed = false;
  scrollLine = 0;
  screen = Screen::Joke;
}

// Tap or BOOT on a joke: the punchline first, then the next joke.
void advance() {
  if (!shown.punchline.isEmpty() && !revealed) {
    revealed = true;
    // Bring the punchline into view if the setup filled the screen.
    const int total = jokeLines().size();
    scrollLine = max(0, total - kVisibleLines);
    return;
  }
  if (shown.fromFavourites) openFavourite(shown.favouriteIndex + 1);
  else openJoke(shownFrom);
}

void toggleFavourite() {
  jokes::setFavourite(shown.id, shown.category, shown.setup, shown.punchline, !jokes::isFavourite(shown.id));
  notifyChanged();
}

void onGesture(Touch::Gesture gesture, uint16_t x, uint16_t y) {
  using G = Touch::Gesture;
  switch (screen) {
    case Screen::Grid:
      if (gesture != G::Tap) return;
      if (nav::hit(x, y) == 1) {
        listPage = 0;
        screen = Screen::Favourites;
      } else if (y > nav::kHeight) {
        const int col = x < kW / 2 ? 0 : 1;
        const int row = y < nav::kHeight + 4 + 76 ? 0 : 1;
        openJoke(kGrid[row * 2 + col]);
      } else {
        return;
      }
      break;
    case Screen::Joke:
      if (gesture == G::SwipeUp) scrollLine += kScrollLines;
      else if (gesture == G::SwipeDown) scrollLine -= kScrollLines;
      else if (gesture != G::Tap) return;
      else if (nav::hit(x, y) == -1) screen = shown.fromFavourites ? Screen::Favourites : Screen::Grid;
      else if (nav::hit(x, y) == 1) toggleFavourite();
      else if (y > nav::kHeight) advance();
      else return;
      break;
    case Screen::Favourites: {
      // Pages: swipe left or up for the next, right or down for the previous.
      if (gesture == G::SwipeUp || gesture == G::SwipeLeft) {
        listPage++;
        break;
      }
      if (gesture == G::SwipeDown || gesture == G::SwipeRight) {
        listPage--;
        break;
      }
      if (gesture != G::Tap) return;
      if (nav::hit(x, y) == -1) {
        screen = Screen::Grid;
        break;
      }
      const auto &favs = jokes::favourites();
      const int pages = max<int>(1, (favs.size() + kListRows - 1) / kListRows);
      if (nav::hit(x, y) == 1 && pages > 1) {  // the page number: next, round
        listPage = (listPage + 1) % pages;
        break;
      }
      if (favs.empty() || y < nav::kHeight + 2) return;
      const int perPage = kListRows;
      const int row = (y - nav::kHeight - 2) / kRowH;
      if (row >= perPage) return;
      const int i = listPage * perPage + row;
      if (i < static_cast<int>(favs.size())) openFavourite(i);
      break;
    }
    case Screen::Message:
      if (gesture != G::Tap || nav::hit(x, y) != -1) return;
      screen = Screen::Grid;
      break;
  }
  redraw = true;
}

// ---------- lock screen ----------

// Lines of a joke as the lock screen sets them: setup regular, punchline bold, centred.
// Measured on `gfx` (the lock widget's own canvas), so the display's font is left alone.
std::vector<Line> lockLines(Adafruit_GFX &gfx, const char *setup, const char *punchline, int maxLines) {
  std::vector<Line> lines;
  gfx.setFont(&FreeSans9pt7b);
  for (const String &l : ui::wrapText(gfx, setup, kTextW, maxLines)) lines.push_back({l});
  if (punchline[0]) {
    gfx.setFont(&FreeSansBold9pt7b);
    for (const String &l : ui::wrapText(gfx, punchline, kTextW, maxLines)) lines.push_back({l, true});
  }
  return lines;
}

// Lines from the top of the widget's canvas; returns the height used (the shell centres it
// with the clock).
int16_t drawLockJoke(Adafruit_GFX &gfx, const tm &now, int16_t maxHeight) {
  const jokes::Joke *j = jokes::lockJoke(now);
  if (!j) return 0;
  const std::vector<Line> lines = lockLines(gfx, j->setup, j->punchline, 4);
  for (size_t i = 0; i < lines.size(); i++) {
    gfx.setFont(lines[i].bold ? &FreeSansBold9pt7b : &FreeSans9pt7b);
    ui::drawCentered(gfx, lines[i].text, 14 + i * kLineH);
  }
  return min<int16_t>(maxHeight, lines.size() * kLineH - 1);
}

void measureLockFit() {
  // 4 lines at most, nothing cut off (asking for 5 shows when it needs more).
  jokes::measureLockFit([](const jokes::Joke &j) {
    epd.setFont(&FreeSans9pt7b);
    int n = ui::wrapText(epd, j.setup, kTextW, 5).size();
    if (j.twoPart()) {
      epd.setFont(&FreeSansBold9pt7b);
      n += ui::wrapText(epd, j.punchline, kTextW, 5).size();
    }
    return n <= 4;
  });
}

// ---------- app (BLE) ----------

void fail(JsonObject reply, const char *error) {
  reply["ok"] = false;
  reply["error"] = error;
}

void registerCommands() {
  ble::on("jokes.status", [](JsonObjectConst, JsonObject reply) {
    reply["count"] = jokes::count();
    JsonObject unseen = reply["unseen"].to<JsonObject>();
    for (int c : kGrid) unseen[jokes::categoryName(c)] = jokes::unseen(c);
    reply["favourites"] = jokes::favourites().size();
    const jokes::SyncState sync = jokes::syncState();
    reply["syncing"] = sync.running;
    reply["done"] = sync.done;
    reply["total"] = sync.total;
    reply["syncedAt"] = sync.syncedAt;  // local time, epoch seconds
    if (sync.error.length()) reply["syncError"] = sync.error;
  });
  ble::on("jokes.favourites", [](JsonObjectConst, JsonObject reply) {
    JsonArray list = reply["favourites"].to<JsonArray>();
    for (const jokes::Favourite &f : jokes::favourites()) {
      JsonObject o = list.add<JsonObject>();
      o["id"] = f.id;
      o["category"] = jokes::categoryName(f.category);
      o["setup"] = f.setup;
      o["punchline"] = f.punchline;
      o["saved"] = f.saved;
    }
  });
  ble::on("jokes.favourite.remove", [](JsonObjectConst args, JsonObject reply) {
    const uint16_t id = args["id"] | 0;
    if (!jokes::isFavourite(id)) return fail(reply, "not a favourite");
    jokes::setFavourite(id, 0, "", "", false);
    if (screen == Screen::Favourites || (screen == Screen::Joke && shown.id == id)) redraw = true;
  });
  ble::on("jokes.sync", [](JsonObjectConst, JsonObject reply) {
    if (net::saved().empty()) return fail(reply, "Dotty has no Wi-Fi yet");
    jokes::startSync();
  });
}

const shell::Picture kOffPictures[] = {
    {kSleepPortrait, kSleepPortraitWidth, kSleepPortraitHeight},
    {kSleepPanda, kSleepPandaWidth, kSleepPandaHeight},
};

}  // namespace

void setup() {
  shell::Config config;
  config.drawApp = drawApp;
  config.drawLockWidget = drawLockJoke;
  config.offPictures = kOffPictures;
  config.offPictureCount = sizeof(kOffPictures) / sizeof(kOffPictures[0]);
  shell::begin(config);

  sdReady = storage::begin();
  if (sdReady && jokes::begin()) measureLockFit();
  registerCommands();
  shell::showApp();
}

void loop() {
  power::setWakeLock(power::kWakeLockNetwork, jokes::syncState().running);

  shell::Input input;
  const bool unlocked = shell::update(input);

  // Download: right away when the card has no jokes, then weekly (in the background).
  if (sdReady && jokes::syncDue()) {
    jokes::startSync();
    if (screen == Screen::Message) redraw = true;
  }
  if (jokes::takeSyncFinished()) {
    measureLockFit();
    notifyChanged();
    if (screen == Screen::Message && jokes::count()) openJoke(waitingCategory);
    redraw = true;
  }
  if (!unlocked) return;

  if (input.gesture != Touch::Gesture::None) onGesture(input.gesture, shell::touch.x(), shell::touch.y());
  if (input.boot) {
    if (screen == Screen::Joke) advance();
    else if (screen == Screen::Grid) openJoke(jokes::kAny);
    redraw = true;
  }

  // Show download progress now and then.
  static uint32_t lastProgress = 0;
  if (screen == Screen::Message && jokes::syncState().running && millis() - lastProgress > 3000) {
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
