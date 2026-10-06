// Album Viewer: photos from the iPhone, already cropped and dithered to 1 bit by the app,
// kept on the SD card in albums (album_library.h). The phone manages them over BLE and
// uploads photos over Wi-Fi (transfer.*).
//
// Screens: the viewer (one photo, the whole screen; swipe or BOOT for the next one, tap
// for the album, position and date) and the album menu ("All photos" + every album).
// Locked, the screen becomes a screensaver: the active album, a new photo every 30 min
// (settable), looping, or a single chosen photo, with a small clock in the corner.

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Preferences.h>
#include <mbedtls/base64.h>

#include <algorithm>

#include "album_library.h"
#include "art.h"
#include "cartridge.h"
#include "core_ble.h"
#include "images/sleep_panda.h"
#include "images/sleep_portrait.h"
#include "log.h"
#include "nav_bar.h"
#include "shell.h"
#include "storage.h"
#include "transfer.h"
#include "ui.h"

DOTTY_CARTRIDGE("album", "Album Viewer", "0.3.2");

namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;
constexpr int16_t kNavH = nav::kHeight;
constexpr int16_t kNavButton = nav::kTouch;  // corner tap width
constexpr int16_t kRowH = 38;  // album menu rows, from kNavH + 2
constexpr int kMenuRows = (kW - kNavH - 2) / kRowH;
constexpr int16_t kInfoH = 26;           // the viewer's bottom strip (position + date)
constexpr uint32_t kOverlayMs = 6000;    // the viewer's labels hide by themselves
constexpr uint32_t kResultMs = 4000;     // "Photos received" after a transfer

bool sdReady = false;

// The active album ("" = every photo), its photos and the one on screen.
String activeAlbum;
std::vector<String> list;
int index = 0;

// Locked: the active album as a slideshow, or always one photo. The slideshow changes photo
// every `slideMinutes`, on the clock (:00, :30…): each change is a full refresh, and those
// are what wear e-paper; 30 min costs no more than the clock's own ghost-clearing refresh.
enum class Saver { Album, Photo };
Saver saver = Saver::Album;
String saverPhoto;
int slideMinutes = 30;

enum class Screen { Viewer, Albums };
Screen screen = Screen::Viewer;
int menuPage = 0;
uint32_t overlayUntil = 0;  // millis; 0 = the photo alone

transfer::Summary lastTransfer;
uint32_t resultUntil = 0;

// The bitmap of the last photo read from the card.
uint8_t bitmap[album::kBitmapBytes];
String bitmapName;

bool stateDirty = true;  // tell the app, save, redraw
String shownPhoto;       // the photo the screen shows now (a different one = full refresh)

// ---------- state ----------

String currentPhoto() {
  return list.empty() ? String() : list[index];
}

String albumLabel(const String &name) {
  if (name.isEmpty()) return "All photos";
  const String label = ui::printable(name);
  return label.isEmpty() ? String("Album") : label;
}

void saveState() {
  Preferences p;
  p.begin("album", false);
  p.putString("active", activeAlbum);
  p.putString("photo", currentPhoto());
  p.putString("saver", saver == Saver::Photo ? "photo" : "album");
  p.putString("saverPhoto", saverPhoto);
  p.putInt("every", slideMinutes);
  p.end();
}

// Loads an album, staying on `keep` if it's in it (else the first photo).
void openAlbum(const String &name, const String &keep = String()) {
  activeAlbum = name.length() && album::albumExists(name) ? name : String();
  list = album::albumPhotos(activeAlbum);
  const auto it = std::find(list.begin(), list.end(), keep);
  index = it == list.end() ? 0 : it - list.begin();
  stateDirty = true;
}

void step(int by) {
  if (list.empty()) return;
  const int count = list.size();
  index = ((index + by) % count + count) % count;
  stateDirty = true;
}

// After library changes: same album and photo when they still exist.
void reload() {
  openAlbum(activeAlbum, currentPhoto());
  if (saverPhoto.length() && !album::hasPhoto(saverPhoto)) {
    saverPhoto = "";
    saver = Saver::Album;
  }
  saveState();
}

bool loadBitmap(const String &name) {
  if (name.isEmpty()) return false;
  if (name == bitmapName) return true;
  bitmapName = album::loadPhoto(name, bitmap) ? name : String();
  return bitmapName.length();
}

// ---------- screens ----------

void drawPhoto(Adafruit_GFX &gfx, const String &name) {
  gfx.fillScreen(kWhite);
  if (loadBitmap(name)) gfx.drawBitmap(0, 0, bitmap, kW, kW, kBlack);
}

void drawViewer() {
  if (list.empty()) {
    epd.fillScreen(kWhite);
    nav::draw(epd, albumLabel(activeAlbum), nav::Icon::Back, nav::Icon::None);
    epd.setTextColor(kBlack);
    epd.setFont(&FreeSansBold9pt7b);
    if (!sdReady) {
      ui::drawCentered(epd, "No SD card", 105);
      return;
    }
    ui::drawCentered(epd, activeAlbum.length() ? "Empty album" : "No photos yet", 92);
    epd.setFont(&FreeSans9pt7b);
    ui::drawCentered(epd, "Add some with", 122);
    ui::drawCentered(epd, "the Dotty app", 142);
    return;
  }
  drawPhoto(epd, currentPhoto());
  if (!overlayUntil) return;
  // The album on top (back = album menu), position and date at the bottom.
  nav::draw(epd, albumLabel(activeAlbum), nav::Icon::Back, nav::Icon::None);
  epd.fillRect(0, kW - kInfoH, kW, kInfoH, kWhite);
  epd.drawFastHLine(0, kW - kInfoH, kW, kBlack);
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSans9pt7b);
  // "2 / 4" on the left, the date taken on the right.
  epd.setCursor(8, kW - 8);
  epd.print(String(index + 1) + " / " + String(list.size()));
  const String date = album::dateTaken(currentPhoto());
  epd.setCursor(kW - 8 - ui::textWidth(epd, date), kW - 8);
  epd.print(date);
}

// The menu's items: "" = All photos, then every album.
std::vector<String> menuItems() {
  std::vector<String> items{""};
  for (const String &name : album::albums()) items.push_back(name);
  return items;
}

// Every row holds an item; the page ("1/2") sits in the nav bar's right corner.
int menuPages(int count) {
  return max(1, (count + kMenuRows - 1) / kMenuRows);
}

void drawAlbums() {
  epd.fillScreen(kWhite);
  const std::vector<String> items = menuItems();
  const int perPage = kMenuRows;
  const int pages = menuPages(items.size());
  menuPage = constrain(menuPage, 0, pages - 1);
  nav::draw(epd, "Albums", nav::Icon::Back, nav::Icon::None, nav::pageLabel(menuPage, pages));
  epd.setFont(&FreeSans9pt7b);
  for (int row = 0; row < perPage; row++) {
    const int i = menuPage * perPage + row;
    if (i >= static_cast<int>(items.size())) break;
    const int16_t top = kNavH + 2 + row * kRowH;
    const bool current = items[i] == activeAlbum;
    if (current) epd.fillRect(0, top, kW, kRowH - 1, kBlack);
    epd.setTextColor(current ? kWhite : kBlack);
    const String count = String(items[i].isEmpty() ? album::photos().size() : album::albumPhotos(items[i]).size());
    const int16_t countW = ui::textWidth(epd, count);
    epd.setCursor(10, top + kRowH / 2 + 6);
    epd.print(ui::fitText(epd, albumLabel(items[i]), kW - 30 - countW));
    epd.setCursor(kW - 10 - countW, top + kRowH / 2 + 6);
    epd.print(count);
    if (!current) epd.drawFastHLine(10, top + kRowH - 1, kW - 20, kBlack);
  }
  epd.setTextColor(kBlack);
}

void drawTransfer() {
  const transfer::Status s = transfer::status();
  epd.fillScreen(kWhite);
  ui::drawHeader(epd, "Receiving photos");
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSansBold24pt7b);
  ui::drawCentered(epd, String(s.filesReceived), 120);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, s.filesReceived == 1 ? "photo received" : "photos received", 150);
}

void drawTransferResult() {
  const transfer::Summary &t = lastTransfer;
  epd.fillScreen(kWhite);
  ui::drawHeader(epd, t.ok() ? "Photos received" : "Transfer failed");
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSansBold9pt7b);
  ui::drawCentered(epd, String(t.received) + (t.received == 1 ? " photo added" : " photos added"), t.ok() ? 100 : 80);
  if (t.ok()) return;
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, "Open the Dotty app", 130);
  ui::drawCentered(epd, "to send the rest", 150);
}

void drawApp() {
  shownPhoto = "";
  if (transfer::active()) drawTransfer();
  else if (resultUntil) drawTransferResult();
  else if (screen == Screen::Albums) drawAlbums();
  else {
    drawViewer();
    shownPhoto = currentPhoto();
  }
}

// ---------- lock screen ----------

// Set while unlocked: the next lock screen starts a new slideshow from the photo on screen.
bool slideshowFresh = true;
int64_t slideshowStartMinute = 0;  // in slideMinutes slots
int slideshowStartIndex = 0;
String lockShown;  // the photo on the lock screen, to know when it changes (full refresh)

int64_t epochMinute(const tm &t) {
  tm copy = t;
  return static_cast<int64_t>(mktime(&copy)) / 60;  // TZ is UTC: local time as is
}

// A small white label in the bottom-right corner: padlock + "18:24" (+ battery when low or
// on a charger).
void drawClockBadge(Adafruit_GFX &gfx, const LockScreenInfo &info) {
  char hhmm[6] = "--:--";
  if (info.timeValid) snprintf(hhmm, sizeof(hhmm), "%02d:%02d", info.time.tm_hour, info.time.tm_min);
  gfx.setFont(&FreeSansBold9pt7b);
  int16_t x1, y1;
  uint16_t tw, th;
  gfx.getTextBounds(hhmm, 0, 0, &x1, &y1, &tw, &th);
  constexpr int16_t kLockH = 13, kPad = 6, kH = 24;
  const bool battery = info.batteryLow || info.externalPower;
  const int16_t lockW = art::padlockWidth(kLockH);
  const int16_t w = kPad + lockW + 5 + tw + (battery ? 6 + 22 : 0) + kPad;
  const int16_t x = kW - w - 3, y = kW - kH - 3;
  gfx.fillRoundRect(x - 1, y - 1, w + 2, kH + 2, 7, kWhite);
  gfx.drawRoundRect(x, y, w, kH, 6, kBlack);
  art::drawSmallPadlock(gfx, x + kPad, y + (kH - kLockH) / 2, kLockH);
  gfx.setTextColor(kBlack);
  gfx.setCursor(x + kPad + lockW + 5 - x1, y + 17);
  gfx.print(hhmm);
  if (battery) ui::drawBattery(gfx, x + w - kPad - 22, y + 7, info.batteryPercent, info.charging);
}

bool drawLockScreenPhoto(Adafruit_GFX &gfx, const LockScreenInfo &info) {
  String photo;
  if (saver == Saver::Photo && album::hasPhoto(saverPhoto)) {
    photo = saverPhoto;
  } else if (!list.empty()) {
    // The active album from the photo that was on screen, a new one every slideMinutes on
    // the clock, looping.
    const int64_t slot = (info.timeValid ? epochMinute(info.time) : 0) / slideMinutes;
    if (slideshowFresh) {
      slideshowFresh = false;
      slideshowStartMinute = slot;
      slideshowStartIndex = index;
    }
    const int count = list.size();
    const int64_t steps = max<int64_t>(0, slot - slideshowStartMinute);
    index = (slideshowStartIndex + steps) % count;  // unlocking shows the same photo
    photo = list[index];
  }
  if (photo.isEmpty() || !loadBitmap(photo)) {
    drawLockScreen(gfx, info);  // nothing to show: the usual clock
    const bool changed = lockShown.length();
    lockShown = "";
    return changed;
  }
  gfx.fillScreen(kWhite);
  gfx.drawBitmap(0, 0, bitmap, kW, kW, kBlack);
  drawClockBadge(gfx, info);
  const bool changed = photo != lockShown;
  lockShown = photo;
  return changed;
}

// ---------- touch ----------

bool onAlbumsTap(uint16_t x, uint16_t y) {
  const std::vector<String> items = menuItems();
  if (y < kNavH) {
    if (x < kNavButton) {
      screen = Screen::Viewer;  // back
    } else if (x > kW - kNavButton && menuPages(items.size()) > 1) {
      menuPage = (menuPage + 1) % menuPages(items.size());  // the page number: next, round
    } else {
      return false;
    }
    return true;
  }
  const int perPage = kMenuRows;
  const int row = (y - kNavH - 2) / kRowH;
  if (row >= perPage) return false;
  const int i = menuPage * perPage + row;
  if (row < 0 || i >= static_cast<int>(items.size())) return false;
  openAlbum(items[i]);
  saveState();
  screen = Screen::Viewer;
  overlayUntil = millis() + kOverlayMs;  // show which album it is
  return true;
}

// Returns true when the screen needs a redraw.
bool onViewerTap(uint16_t x, uint16_t y) {
  const bool navShown = overlayUntil || list.empty();
  if (navShown && y < kNavH && x < kNavButton) {
    screen = Screen::Albums;
    overlayUntil = 0;
    const std::vector<String> items = menuItems();
    const auto it = std::find(items.begin(), items.end(), activeAlbum);
    menuPage = (it - items.begin()) / kMenuRows;
    return true;
  }
  if (list.empty()) return false;
  overlayUntil = overlayUntil ? 0 : millis() + kOverlayMs;
  return true;
}

// ---------- app (BLE) ----------

void addState(JsonObject out) {
  out["album"] = activeAlbum;
  out["index"] = index;
  out["count"] = list.size();
  out["photo"] = currentPhoto();
  JsonObject s = out["screensaver"].to<JsonObject>();
  s["mode"] = saver == Saver::Photo ? "photo" : "album";
  if (saver == Saver::Photo) s["photo"] = saverPhoto;
  s["every"] = slideMinutes;
}

void notifyState() {
  JsonDocument event;
  event["event"] = "album.state";
  addState(event.as<JsonObject>());
  ble::notify(event);
}

void notifyLibraryChanged() {
  JsonDocument event;
  event["event"] = "album.library";
  ble::notify(event);
}

std::vector<String> stringList(JsonVariantConst value) {
  std::vector<String> out;
  for (JsonVariantConst v : value.as<JsonArrayConst>()) out.push_back(v | "");
  return out;
}

void fail(JsonObject reply, const char *error) {
  reply["ok"] = false;
  reply["error"] = error;
}

// The library changed (from the app): reload, redraw, tell the app.
void libraryChanged() {
  reload();
  notifyLibraryChanged();
}

void registerCommands() {
  ble::on("album.status", [](JsonObjectConst, JsonObject reply) {
    addState(reply);
    reply["photos"] = album::photos().size();
    reply["albums"] = album::albums().size();
  });
  ble::on("album.library", [](JsonObjectConst, JsonObject reply) {
    JsonArray photos = reply["photos"].to<JsonArray>();
    for (const album::Photo &p : album::photos()) {
      JsonObject o = photos.add<JsonObject>();
      o["name"] = p.name;
      o["added"] = static_cast<int64_t>(p.added);  // local time as epoch seconds
    }
    JsonArray albums = reply["albums"].to<JsonArray>();
    for (const String &name : album::albums()) {
      const std::vector<String> inside = album::albumPhotos(name);
      JsonObject o = albums.add<JsonObject>();
      o["name"] = name;
      o["count"] = inside.size();
      if (!inside.empty()) o["cover"] = inside.front();
    }
  });
  ble::on("album.album", [](JsonObjectConst args, JsonObject reply) {
    const String name = args["name"] | "";
    if (name.length() && !album::albumExists(name)) return fail(reply, "no such album");
    JsonArray photos = reply["photos"].to<JsonArray>();
    for (const String &p : album::albumPhotos(name)) photos.add(p);
  });
  // {name} → {name, data}: the 5000-byte bitmap in base64 (the app's thumbnails).
  ble::on("album.photo", [](JsonObjectConst args, JsonObject reply) {
    const String name = args["name"] | "";
    static uint8_t raw[album::kBitmapBytes];
    if (!album::loadPhoto(name, raw)) return fail(reply, "no such photo");
    static unsigned char text[album::kBitmapBytes * 4 / 3 + 8];
    size_t length = 0;
    mbedtls_base64_encode(text, sizeof(text), &length, raw, sizeof(raw));
    reply["name"] = name;
    reply["data"] = reinterpret_cast<const char *>(text);
  });
  // {album?, photo?}: shows that album (and photo) on Dotty.
  ble::on("album.show", [](JsonObjectConst args, JsonObject reply) {
    const String name = args["album"] | activeAlbum;
    if (name.length() && !album::albumExists(name)) return fail(reply, "no such album");
    openAlbum(name, args["photo"] | "");
    saveState();
    screen = Screen::Viewer;
    shell::wake();
    addState(reply);
  });
  // {mode: "album" | "photo", album?, photo?}: what the lock screen shows.
  ble::on("album.screensaver", [](JsonObjectConst args, JsonObject reply) {
    const String mode = args["mode"] | "album";
    if (mode == "photo") {
      const String photo = args["photo"] | "";
      if (!album::hasPhoto(photo)) return fail(reply, "no such photo");
      saver = Saver::Photo;
      saverPhoto = photo;
    } else {
      saver = Saver::Album;
      saverPhoto = "";
      if (!args["album"].isNull()) {
        const String name = args["album"] | "";
        if (name.length() && !album::albumExists(name)) return fail(reply, "no such album");
        if (name != activeAlbum) openAlbum(name);
      }
    }
    saveState();
    stateDirty = true;
    addState(reply);
  });
  // {every}: minutes between slideshow photos on the lock screen (1-1440; the app offers
  // 10, 30, 60).
  ble::on("album.slideshow", [](JsonObjectConst args, JsonObject reply) {
    const int every = args["every"] | 0;
    if (every < 1 || every > 1440) return fail(reply, "every must be 1-1440 minutes");
    slideMinutes = every;
    saveState();
    stateDirty = true;
    addState(reply);
  });
  ble::on("album.create", [](JsonObjectConst args, JsonObject reply) {
    if (!album::createAlbum(args["name"] | "")) return fail(reply, "invalid name");
    notifyLibraryChanged();
  });
  // Deletes the album only: its photos stay on the card.
  ble::on("album.delete", [](JsonObjectConst args, JsonObject) {
    album::deleteAlbum(args["name"] | "");
    libraryChanged();
  });
  ble::on("album.rename", [](JsonObjectConst args, JsonObject reply) {
    const String name = args["name"] | "";
    const String to = args["to"] | "";
    if (!album::renameAlbum(name, to)) return fail(reply, "that name is empty or taken");
    if (activeAlbum == name) activeAlbum = storage::safeName(to);
    libraryChanged();
  });
  ble::on("album.add", [](JsonObjectConst args, JsonObject reply) {
    if (!album::addToAlbum(args["name"] | "", stringList(args["photos"]))) return fail(reply, "no such album");
    libraryChanged();
  });
  ble::on("album.remove", [](JsonObjectConst args, JsonObject) {
    album::removeFromAlbum(args["name"] | "", args["photo"] | "");
    libraryChanged();
  });
  // {name, from, to}: positions as in album.album.
  ble::on("album.move", [](JsonObjectConst args, JsonObject reply) {
    if (!album::moveInAlbum(args["name"] | "", args["from"] | -1, args["to"] | -1)) {
      return fail(reply, "no such position");
    }
    libraryChanged();
  });
  // Deletes photos from the card (and every album). {names: [...]}
  ble::on("album.photo.delete", [](JsonObjectConst args, JsonObject reply) {
    int deleted = 0;
    for (const String &name : stringList(args["names"])) deleted += album::deletePhoto(name);
    reply["deleted"] = deleted;
    if (bitmapName.length() && !album::hasPhoto(bitmapName)) bitmapName = "";
    libraryChanged();
  });

  // Uploads go into photos/; rescan when a transfer session ends.
  transfer::registerCommands([](const transfer::Summary &summary) {
    album::rescan();
    reload();
    notifyLibraryChanged();
    lastTransfer = summary;
    resultUntil = millis() + kResultMs;
    shell::showApp();
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
  config.lockScreen = drawLockScreenPhoto;
  config.offPictures = kOffPictures;
  config.offPictureCount = sizeof(kOffPictures) / sizeof(kOffPictures[0]);
  shell::begin(config);

  sdReady = storage::begin();
  if (sdReady) album::begin();
  Preferences p;
  p.begin("album", true);
  const String active = p.getString("active", "");
  const String photo = p.getString("photo", "");
  saver = p.getString("saver", "album") == "photo" ? Saver::Photo : Saver::Album;
  saverPhoto = p.getString("saverPhoto", "");
  slideMinutes = constrain(p.getInt("every", 30), 1, 1440);
  p.end();
  openAlbum(active, photo);
  registerCommands();
  shell::showApp();
}

void loop() {
  shell::Input input;
  const bool unlocked = shell::update(input);
  transfer::poll();
  if (!unlocked) return;
  if (!slideshowFresh) {  // just unlocked: the slideshow may have moved on
    slideshowFresh = true;  // the next lock starts from the photo on screen
    stateDirty = true;
  }

  static uint32_t lastTransferDraw = 0;
  if (transfer::active()) {
    if (millis() - lastTransferDraw > 1500 && !epd.isBusy()) {
      lastTransferDraw = millis();
      shell::wake();  // keep it unlocked and visible while photos arrive
      drawTransfer();
      shell::refresh(false);
    }
    delay(10);
    return;
  }

  static bool redraw = false;    // partial refresh
  static bool fullDraw = false;  // a new photo: full refresh (partial ones ghost on photos)
  if (resultUntil && (millis() > resultUntil || input.gesture == Touch::Gesture::Tap)) {
    resultUntil = 0;
    redraw = true;
    input.gesture = Touch::Gesture::None;
  }
  if (overlayUntil && millis() > overlayUntil) {
    overlayUntil = 0;
    redraw = true;
  }
  using G = Touch::Gesture;
  if (input.key == 'n') input.boot = true;  // developer aids: next photo…
  if (input.key == 'o' && screen == Screen::Viewer) redraw |= onViewerTap(kW / 2, kW / 2);  // …the labels…
  if (input.key == 'l') shell::lock();      // …and lock (to screenshot the screensaver)
  if (input.gesture == G::Tap) {
    const uint16_t x = shell::touch.x(), y = shell::touch.y();
    redraw |= screen == Screen::Albums ? onAlbumsTap(x, y) : onViewerTap(x, y);
  }
  if (screen == Screen::Viewer && (input.gesture == G::SwipeLeft || input.boot)) step(1);
  if (screen == Screen::Viewer && input.gesture == G::SwipeRight) step(-1);
  if (input.gesture == G::LongPress) fullDraw = true;  // clears ghosting
  if (screen == Screen::Albums) {  // pages: swipe left/up = next, right/down = previous
    if (input.gesture == G::SwipeLeft || input.gesture == G::SwipeUp) menuPage++, redraw = true;
    if (input.gesture == G::SwipeRight || input.gesture == G::SwipeDown) menuPage--, redraw = true;
  }
  if (stateDirty) {
    stateDirty = false;
    redraw = true;
    saveState();
    notifyState();
  }

  if ((redraw || fullDraw) && !epd.isBusy()) {
    const String before = shownPhoto;
    drawApp();
    shell::refresh(fullDraw || (shownPhoto.length() && shownPhoto != before));
    redraw = false;
    fullDraw = false;
  }
  delay(10);
}
