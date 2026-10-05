// Music cartridge: a small music player for the SD card. Songs live in the cartridge's
// library, playlists are lists of library songs (see music_library.h). The phone manages
// the library over BLE and uploads songs over Wi-Fi (transfer.*).
//
// Screens: the player (nav bar: shuffle on the left, playlists on the right) and the
// playlist menu (back on the left; "Play all" + every playlist, paged).

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Preferences.h>

#include <algorithm>
#include <numeric>
#include <random>

#include "audio_player.h"
#include "cartridge.h"
#include "core_ble.h"
#include "images/sleep_panda.h"
#include "images/sleep_portrait.h"
#include "log.h"
#include "music_library.h"
#include "power.h"
#include "shell.h"
#include "storage.h"
#include "transfer.h"
#include "ui.h"

DOTTY_CARTRIDGE("music", "Music", "0.8.5");

namespace {

using shell::epd;

constexpr uint32_t kPlayerRefreshMs = 1000;
constexpr uint32_t kRestartThresholdMs = 3000;  // "previous" restarts the song after this
constexpr uint32_t kStateEventMs = 5000;        // position updates to the app while playing
constexpr uint8_t kVolumePercent = 80;
constexpr uint8_t kVolumeStep = 10;
constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

constexpr int16_t kNavH = 30;     // nav bar height; its corner buttons are kNavButton wide
constexpr int16_t kNavButton = 44;
constexpr int16_t kRowH = 28;     // playlist menu rows, from kNavH + 2
constexpr int kMenuRows = (EpdDisplay::kSize - kNavH - 2) / kRowH;
constexpr int16_t kButtonY = 100;
constexpr int16_t kPrevX = 38;
constexpr int16_t kNextX = kW - 38;
constexpr int16_t kVolumeY = 184;

AudioPlayer player;
bool sdReady = false;

// The queue: the whole library ("" playlist) or one playlist, and the current song.
String queueName;
std::vector<String> queue;
int queueIndex = 0;
bool autoAdvance = false;  // true while a song we started is meant to keep going
bool stateDirty = true;    // tell the app (and redraw)

// Play order over the queue: in order, or shuffled (the current song first).
bool shuffle = false;
std::vector<int> order;
int orderPos = 0;

enum class Screen { Player, Playlists };
Screen screen = Screen::Player;
int menuPage = 0;

// After a Wi-Fi transfer: its result shows for a few seconds, then the player is back.
constexpr uint32_t kResultMs = 4000;
transfer::Summary lastTransfer;
uint32_t resultUntil = 0;

bool musicPlaying() {
  return player.isPlaying() && !player.isPaused();
}

String currentSong() {
  return queue.empty() ? String() : queue[queueIndex];
}

void loadQueue(const String &playlist) {
  queueName = playlist;
  queue.clear();
  if (playlist.isEmpty()) {
    for (const music::Song &s : music::songs()) queue.push_back(s.name);
  } else {
    queue = music::playlistSongs(playlist);
  }
  if (queueIndex >= static_cast<int>(queue.size())) queueIndex = 0;
}

// Rebuilds the play order; with keepCurrent the current song stays where playback is.
void buildOrder(bool keepCurrent) {
  order.resize(queue.size());
  std::iota(order.begin(), order.end(), 0);
  if (shuffle && order.size() > 1) {
    std::minstd_rand rng(esp_random());
    std::shuffle(order.begin(), order.end(), rng);
    if (keepCurrent) std::iter_swap(order.begin(), std::find(order.begin(), order.end(), queueIndex));
  }
  const auto it = std::find(order.begin(), order.end(), queueIndex);
  orderPos = it == order.end() ? 0 : it - order.begin();
}

void playIndex(int index) {
  if (queue.empty()) return;
  const int count = queue.size();
  queueIndex = ((index % count) + count) % count;
  const auto it = std::find(order.begin(), order.end(), queueIndex);
  if (it != order.end()) orderPos = it - order.begin();
  autoAdvance = player.play(music::songPath(queue[queueIndex]).c_str());
  stateDirty = true;
  LOGI("player", "playing %d/%d %s", queueIndex + 1, count, queue[queueIndex].c_str());
}

void togglePlay() {
  if (player.isPlaying()) {
    player.togglePause();
  } else {
    playIndex(queueIndex);
  }
  stateDirty = true;
}

// Plays the song at a position in the play order (wrapping around).
void playOrderPos(int pos) {
  if (order.empty()) return;
  const int count = order.size();
  playIndex(order[((pos % count) + count) % count]);
}

void next() {
  playOrderPos(orderPos + 1);
}

void previous() {
  if (player.isPlaying() && player.positionMs() > kRestartThresholdMs) {
    playIndex(queueIndex);
  } else {
    playOrderPos(orderPos - 1);
  }
}

void setShuffle(bool on) {
  shuffle = on;
  buildOrder(true);
  Preferences prefs;
  prefs.begin("music", false);
  prefs.putBool("shuffle", on);
  prefs.end();
  stateDirty = true;
}

// Switches to the library ("") or a playlist and plays it from its first song.
void playQueue(const String &playlist) {
  loadQueue(playlist);
  queueIndex = 0;
  buildOrder(false);
  playOrderPos(0);
  stateDirty = true;
}

void setVolume(int volume) {
  player.setVolume(constrain(volume, 0, 100));
  stateDirty = true;
}

// After library edits: rebuild the queue, keeping the current song if it still exists.
void refreshQueue() {
  const String keep = currentSong();
  const std::vector<String> lists = music::playlists();
  if (queueName.length() && std::find(lists.begin(), lists.end(), queueName) == lists.end()) queueName = "";
  loadQueue(queueName);
  const auto it = std::find(queue.begin(), queue.end(), keep);
  if (it != queue.end()) {
    queueIndex = it - queue.begin();
  } else if (keep.length() && player.isPlaying()) {
    autoAdvance = false;
    player.stop();  // its file was deleted
  }
  buildOrder(true);
  stateDirty = true;
}

void drawTriangle(int16_t cx, int16_t cy, int16_t dir, uint16_t color) {
  epd.fillTriangle(cx - 8 * dir, cy - 10, cx - 8 * dir, cy + 10, cx + 7 * dir, cy, color);
}

// A song's title as the screen can show it, or `fallback` when nothing printable is left
// (e.g. an all-Korean title: the fonts are ASCII only; the app shows the real one).
String displayTitle(const String &song, const String &fallback) {
  const String title = ui::printable(music::title(song));
  return title.isEmpty() ? fallback : title;
}

// ---------- nav bar ----------

enum class NavIcon { None, Back, Shuffle, InOrder, Playlists };

// A thick white line (the nav bar is black).
void thickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  for (int d = 0; d < 2; d++) {
    epd.drawLine(x0, y0 + d, x1, y1 + d, kWhite);
    epd.drawLine(x0 + d, y0, x1 + d, y1, kWhite);
  }
}

// An arrow head pointing right with its tip at (x, y).
void arrowHead(int16_t x, int16_t y) {
  epd.fillTriangle(x, y, x - 6, y - 5, x - 6, y + 5, kWhite);
}

// Icons are drawn inside the kNavButton-wide corner, centred on cx.
void drawNavIcon(NavIcon icon, int16_t cx) {
  const int16_t cy = kNavH / 2;
  switch (icon) {
    case NavIcon::Back:
      thickLine(cx + 4, cy - 8, cx - 4, cy);
      thickLine(cx - 4, cy, cx + 4, cy + 8);
      break;
    case NavIcon::Shuffle:  // crossing arrows
      thickLine(cx - 11, cy - 7, cx + 6, cy + 6);
      thickLine(cx - 11, cy + 6, cx + 6, cy - 7);
      arrowHead(cx + 11, cy - 7);
      arrowHead(cx + 11, cy + 7);
      break;
    case NavIcon::InOrder:  // parallel arrows
      thickLine(cx - 11, cy - 6, cx + 6, cy - 6);
      thickLine(cx - 11, cy + 5, cx + 6, cy + 5);
      arrowHead(cx + 11, cy - 5);
      arrowHead(cx + 11, cy + 6);
      break;
    case NavIcon::Playlists:  // a list with a note
      for (int i = 0; i < 3; i++) epd.fillRect(cx - 12, cy - 8 + i * 7, i == 2 ? 10 : 16, 3, kWhite);
      epd.fillCircle(cx + 7, cy + 7, 3, kWhite);
      epd.fillRect(cx + 9, cy - 6, 2, 13, kWhite);
      epd.fillRect(cx + 9, cy - 6, 5, 2, kWhite);
      break;
    case NavIcon::None:
      break;
  }
}

void drawNavBar(const String &title, NavIcon left, NavIcon right) {
  epd.fillRect(0, 0, kW, kNavH, kBlack);
  epd.setFont(&FreeSans9pt7b);
  epd.setTextColor(kWhite);
  ui::drawCentered(epd, ui::fitText(epd, title, kW - 2 * kNavButton), 20);
  epd.setTextColor(kBlack);
  drawNavIcon(left, kNavButton / 2);
  drawNavIcon(right, kW - kNavButton / 2);
}

// ---------- screens ----------

void drawPlayer() {
  epd.fillScreen(kWhite);
  drawNavBar(queueName.length() ? queueName : String("Music"), shuffle ? NavIcon::Shuffle : NavIcon::InOrder,
             NavIcon::Playlists);

  epd.setFont(&FreeSansBold9pt7b);
  if (!sdReady) {
    ui::drawCentered(epd, "No SD card", 105);
    return;
  }
  if (queue.empty()) {
    ui::drawCentered(epd, queueName.length() ? "Empty playlist" : "No songs yet", 92);
    epd.setFont(&FreeSans9pt7b);
    ui::drawCentered(epd, "Add some with", 122);
    ui::drawCentered(epd, "the Dotty app", 142);
    return;
  }
  // One line, then the position in the queue; titles with no Latin letters (the fonts have
  // nothing else) get a stand-in.
  ui::drawCentered(epd, ui::fitText(epd, displayTitle(currentSong(), "Song " + String(queueIndex + 1)), kW - 12), 50);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, String(queueIndex + 1) + " / " + String(queue.size()), 69);

  // Previous | play/pause | next.
  const int16_t cx = kW / 2, cy = kButtonY;
  epd.fillCircle(cx, cy, 26, kBlack);
  if (musicPlaying()) {
    epd.fillRect(cx - 10, cy - 11, 7, 22, kWhite);
    epd.fillRect(cx + 3, cy - 11, 7, 22, kWhite);
  } else {
    epd.fillTriangle(cx - 7, cy - 13, cx - 7, cy + 13, cx + 13, cy, kWhite);
  }
  for (int16_t bx : {kPrevX, kNextX}) {
    epd.drawCircle(bx, cy, 20, kBlack);
    epd.drawCircle(bx, cy, 19, kBlack);
  }
  drawTriangle(kPrevX + 2, cy, -1, kBlack);
  epd.fillRect(kPrevX - 9, cy - 10, 3, 20, kBlack);
  drawTriangle(kNextX - 2, cy, 1, kBlack);
  epd.fillRect(kNextX + 7, cy - 10, 3, 20, kBlack);

  // Progress + time.
  const uint32_t pos = player.isPlaying() ? player.positionMs() : 0;
  const uint32_t dur = player.isPlaying() ? player.durationMs() : 0;
  const int16_t barX = 15, barY = 134, barW = kW - 30, barH = 8;
  epd.drawRect(barX, barY, barW, barH, kBlack);
  if (dur > 0) {
    epd.fillRect(barX + 2, barY + 2, static_cast<int64_t>(barW - 4) * min(pos, dur) / dur, barH - 4, kBlack);
  }
  ui::drawCentered(epd, ui::formatDuration(pos) + " / " + ui::formatDuration(dur), 160);

  // Volume:  -   vol 80%   +
  epd.fillRect(16, kVolumeY - 6, 14, 3, kBlack);
  epd.fillRect(kW - 30, kVolumeY - 6, 14, 3, kBlack);
  epd.fillRect(kW - 24, kVolumeY - 12, 3, 14, kBlack);
  ui::drawCentered(epd, "vol " + String(player.volume()) + "%", kVolumeY);
}

void drawTransfer() {
  const transfer::Status s = transfer::status();
  epd.fillScreen(kWhite);
  ui::drawHeader(epd, "Receiving songs");
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSansBold9pt7b);
  const String file = s.file.length() ? displayTitle(s.file, "Song " + String(s.filesReceived + 1))
                                      : String("Waiting for songs");
  ui::drawWrapped(epd, file, 66, kW - 12, 2, 18);
  const int16_t barX = 15, barY = 105, barW = kW - 30, barH = 10;
  epd.drawRect(barX, barY, barW, barH, kBlack);
  if (s.total > 0) {
    epd.fillRect(barX + 2, barY + 2, static_cast<int64_t>(barW - 4) * s.done / s.total, barH - 4, kBlack);
  }
  epd.setFont(&FreeSans9pt7b);
  if (s.total > 0 && s.file.length()) {
    // "1.6 / 4.0 MB" and the speed since the previous screen update.
    static size_t lastDone = 0;
    static uint32_t lastAt = 0;
    const uint32_t now = millis();
    const float rate = s.done > lastDone && lastAt ? (s.done - lastDone) / 1.024f / (now - lastAt) : 0;
    lastDone = s.done;
    lastAt = now;
    char line[40];
    snprintf(line, sizeof(line), "%.1f / %.1f MB", s.done / 1048576.0f, s.total / 1048576.0f);
    ui::drawCentered(epd, line, 138);
    if (rate > 0) {
      snprintf(line, sizeof(line), "%.0f KB/s", rate);
      ui::drawCentered(epd, line, 158);
    }
  }
  ui::drawCentered(epd, String(s.filesReceived) + (s.filesReceived == 1 ? " song received" : " songs received"), 186);
}

void drawTransferResult() {
  const transfer::Summary &t = lastTransfer;
  epd.fillScreen(kWhite);
  ui::drawHeader(epd, t.ok() ? "Songs received" : "Transfer failed");
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSansBold9pt7b);
  const String count = String(t.received) + (t.received == 1 ? " song added" : " songs added");
  ui::drawCentered(epd, count, t.ok() ? 100 : 70);
  epd.setFont(&FreeSans9pt7b);
  if (t.ok()) return;
  if (t.failed > 0) {
    ui::drawCentered(epd, String(t.failed) + (t.failed == 1 ? " upload broke off" : " uploads broke off"), 105);
  } else {
    ui::drawCentered(epd, "The app stopped", 105);
    ui::drawCentered(epd, "sending", 125);
  }
  ui::drawCentered(epd, "Open the Dotty app", 160);
  ui::drawCentered(epd, "to send the rest", 180);
}

// The menu's items: "" = Play all (the library), then every playlist.
std::vector<String> menuItems() {
  std::vector<String> items{""};
  for (const String &name : music::playlists()) items.push_back(name);
  return items;
}

// Items per page: every row, or one row fewer when a pager row is needed.
int menuPerPage(int count) {
  return count <= kMenuRows ? kMenuRows : kMenuRows - 1;
}

void drawPlaylists() {
  epd.fillScreen(kWhite);
  drawNavBar("Playlists", NavIcon::Back, NavIcon::None);
  const std::vector<String> items = menuItems();
  const int perPage = menuPerPage(items.size());
  const int pages = (items.size() + perPage - 1) / perPage;
  menuPage = constrain(menuPage, 0, pages - 1);
  epd.setFont(&FreeSans9pt7b);
  for (int row = 0; row < perPage; row++) {
    const int i = menuPage * perPage + row;
    if (i >= static_cast<int>(items.size())) break;
    const int16_t top = kNavH + 2 + row * kRowH;
    const bool current = items[i] == queueName;  // the queue playing now is highlighted
    if (current) epd.fillRect(0, top, kW, kRowH - 1, kBlack);
    epd.setTextColor(current ? kWhite : kBlack);
    const String label = items[i].isEmpty() ? String("Play all")
                         : ui::printable(items[i]).isEmpty() ? "Playlist " + String(i) : items[i];
    const String count = String(items[i].isEmpty() ? music::songs().size() : music::playlistSongs(items[i]).size());
    const int16_t countW = ui::textWidth(epd, count);
    epd.setCursor(10, top + 19);
    epd.print(ui::fitText(epd, label, kW - 30 - countW));
    epd.setCursor(kW - 10 - countW, top + 19);
    epd.print(count);
    if (!current) epd.drawFastHLine(10, top + kRowH - 1, kW - 20, kBlack);
  }
  epd.setTextColor(kBlack);
  if (pages > 1) {  // pager:  <   2 / 3   >
    const int16_t top = kNavH + 2 + perPage * kRowH;
    ui::drawCentered(epd, String(menuPage + 1) + " / " + String(pages), top + 19);
    if (menuPage > 0) epd.fillTriangle(22, top + 13, 32, top + 6, 32, top + 20, kBlack);
    if (menuPage < pages - 1) epd.fillTriangle(kW - 22, top + 13, kW - 32, top + 6, kW - 32, top + 20, kBlack);
  }
}

void drawApp() {
  if (transfer::active()) drawTransfer();
  else if (resultUntil) drawTransferResult();
  else if (screen == Screen::Playlists) drawPlaylists();
  else drawPlayer();
}

// ---------- touch ----------

// Returns true when the screen needs a redraw.
bool onPlaylistsTap(uint16_t x, uint16_t y) {
  if (y < kNavH) {
    if (x >= kNavButton) return false;
    screen = Screen::Player;  // back
    return true;
  }
  const std::vector<String> items = menuItems();
  const int perPage = menuPerPage(items.size());
  const int row = (y - kNavH - 2) / kRowH;
  if (row >= perPage) {  // pager row
    if (x < 70) menuPage--;
    else if (x > kW - 70) menuPage++;
    else return false;
    return true;  // drawPlaylists clamps the page
  }
  const int i = menuPage * perPage + row;
  if (row < 0 || i >= static_cast<int>(items.size())) return false;
  playQueue(items[i]);
  screen = Screen::Player;
  return true;
}

bool onTap(uint16_t x, uint16_t y) {
  if (!sdReady) return false;
  if (screen == Screen::Playlists) return onPlaylistsTap(x, y);
  if (y < kNavH) {
    if (x < kNavButton) {
      setShuffle(!shuffle);
    } else if (x > kW - kNavButton) {
      screen = Screen::Playlists;
      menuPage = 0;
    } else {
      return false;
    }
    return true;
  }
  if (y > kVolumeY - 24) {  // volume row
    if (x < 60) setVolume(player.volume() - kVolumeStep);
    else if (x > kW - 60) setVolume(player.volume() + kVolumeStep);
    else return false;
    return true;
  }
  if (queue.empty() || y < kButtonY - 35 || y > kButtonY + 35) return false;
  if (x < 70) previous();
  else if (x > kW - 70) next();
  else togglePlay();
  return false;  // state changes are picked up by the loop
}

// ---------- app (BLE) ----------

void addState(JsonObject out) {
  out["queue"] = queueName;
  out["index"] = queueIndex;
  out["count"] = queue.size();
  out["song"] = currentSong();
  out["title"] = music::title(currentSong());
  out["playing"] = musicPlaying();
  out["paused"] = player.isPaused();
  out["position"] = player.isPlaying() ? player.positionMs() / 1000 : 0;
  out["duration"] = player.isPlaying() ? player.durationMs() / 1000 : 0;
  out["volume"] = player.volume();
  out["shuffle"] = shuffle;
}

// music.state on every change, and every few seconds while playing (position).
void notifyState() {
  static uint32_t lastSent = 0;
  static bool pending = false;
  pending |= stateDirty;
  if (!pending && !(musicPlaying() && millis() - lastSent > kStateEventMs)) return;
  pending = false;
  lastSent = millis();
  JsonDocument event;
  event["event"] = "music.state";
  addState(event.as<JsonObject>());
  ble::notify(event);
}

void notifyLibraryChanged() {
  JsonDocument event;
  event["event"] = "music.library";
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

void registerCommands() {
  ble::on("music.status", [](JsonObjectConst, JsonObject reply) { addState(reply); });
  ble::on("music.toggle", [](JsonObjectConst, JsonObject reply) {
    togglePlay();
    addState(reply);
  });
  ble::on("music.next", [](JsonObjectConst, JsonObject reply) {
    next();
    addState(reply);
  });
  ble::on("music.prev", [](JsonObjectConst, JsonObject reply) {
    previous();
    addState(reply);
  });
  ble::on("music.shuffle", [](JsonObjectConst args, JsonObject reply) {
    setShuffle(args["on"] | !shuffle);
    addState(reply);
  });
  ble::on("music.volume", [](JsonObjectConst args, JsonObject reply) {
    const int value = args["value"] | -1;
    if (value < 0 || value > 100) return fail(reply, "value must be 0-100");
    setVolume(value);
    addState(reply);
  });
  // {playlist?: "" = library, index?, song?}: plays from that queue.
  ble::on("music.play", [](JsonObjectConst args, JsonObject reply) {
    loadQueue(args["playlist"] | "");
    int index = args["index"] | 0;
    const String song = args["song"] | "";
    if (song.length()) {
      const auto it = std::find(queue.begin(), queue.end(), song);
      if (it == queue.end()) return fail(reply, "song not in that list");
      index = it - queue.begin();
    }
    if (queue.empty()) return fail(reply, "nothing to play");
    queueIndex = index;
    buildOrder(true);  // shuffled: the rest follows this song in a new random order
    playIndex(index);
    addState(reply);
  });

  ble::on("music.library", [](JsonObjectConst, JsonObject reply) {
    JsonArray songs = reply["songs"].to<JsonArray>();
    for (const music::Song &s : music::songs()) {
      JsonObject o = songs.add<JsonObject>();
      o["name"] = s.name;
      o["title"] = music::title(s.name);
      o["size"] = s.size;
      o["added"] = static_cast<int64_t>(s.added);  // local time as epoch seconds
    }
    JsonArray lists = reply["playlists"].to<JsonArray>();
    for (const String &name : music::playlists()) {
      JsonObject o = lists.add<JsonObject>();
      o["name"] = name;
      o["count"] = music::playlistSongs(name).size();
    }
  });
  ble::on("music.playlist", [](JsonObjectConst args, JsonObject reply) {
    JsonArray songs = reply["songs"].to<JsonArray>();
    for (const String &s : music::playlistSongs(args["name"] | "")) songs.add(s);
  });
  ble::on("music.playlist.create", [](JsonObjectConst args, JsonObject reply) {
    if (!music::createPlaylist(args["name"] | "")) return fail(reply, "invalid name");
    notifyLibraryChanged();
  });
  ble::on("music.playlist.delete", [](JsonObjectConst args, JsonObject) {
    music::deletePlaylist(args["name"] | "");
    refreshQueue();
    notifyLibraryChanged();
  });
  ble::on("music.playlist.rename", [](JsonObjectConst args, JsonObject reply) {
    const String name = args["name"] | "";
    const String to = args["to"] | "";
    if (!music::renamePlaylist(name, to)) return fail(reply, "that name is empty or taken");
    if (queueName == name) queueName = storage::safeName(to);
    refreshQueue();
    notifyLibraryChanged();
  });
  ble::on("music.playlist.add", [](JsonObjectConst args, JsonObject reply) {
    if (!music::addToPlaylist(args["name"] | "", stringList(args["songs"]))) return fail(reply, "no such playlist");
    refreshQueue();
    notifyLibraryChanged();
  });
  // {name, from, to}: positions as in music.playlist.
  ble::on("music.playlist.move", [](JsonObjectConst args, JsonObject reply) {
    if (!music::moveInPlaylist(args["name"] | "", args["from"] | -1, args["to"] | -1)) {
      return fail(reply, "no such position");
    }
    refreshQueue();
    notifyLibraryChanged();
  });
  ble::on("music.playlist.remove", [](JsonObjectConst args, JsonObject) {
    music::removeFromPlaylist(args["name"] | "", args["song"] | "");
    refreshQueue();
    notifyLibraryChanged();
  });
  ble::on("music.song.delete", [](JsonObjectConst args, JsonObject reply) {
    if (!music::deleteSong(args["name"] | "")) return fail(reply, "no such song");
    refreshQueue();
    notifyLibraryChanged();
  });

  // Uploads go into the library folder; rescan when a transfer session ends.
  transfer::registerCommands([](const transfer::Summary &summary) {
    music::rescan();
    refreshQueue();
    notifyLibraryChanged();
    lastTransfer = summary;
    resultUntil = millis() + kResultMs;
    shell::showApp();
  });
}

// ---------- loop helpers ----------

// Redraws on any state change, and once a second while playing.
void loopPlayer(bool redraw, bool fullRequested) {
  static uint32_t lastDraw = 0;
  static uint32_t lastPositionS = UINT32_MAX;
  static bool lastPlaying = false, lastPaused = false;
  static String lastSong;

  const bool playing = player.isPlaying(), paused = player.isPaused();
  const uint32_t positionS = player.positionMs() / 1000;
  const bool changed = playing != lastPlaying || paused != lastPaused || currentSong() != lastSong;
  const bool tick = screen == Screen::Player && !resultUntil && playing && !paused && positionS != lastPositionS &&
                    millis() - lastDraw >= kPlayerRefreshMs;
  if (fullRequested || redraw || changed || tick) {
    lastPlaying = playing;
    lastPaused = paused;
    lastPositionS = positionS;
    lastSong = currentSong();
    lastDraw = millis();
    drawApp();
    shell::refresh(fullRequested);
  }
}

// The receiving screen while a transfer runs (partial refresh every 1.5 s).
void loopTransfer() {
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 1500 && !epd.isBusy()) {
    lastDraw = millis();
    shell::wake();  // keep it unlocked and visible while songs arrive
    drawTransfer();
    shell::refresh(false);
  }
}

const shell::Picture kOffPictures[] = {
    {kSleepPortrait, kSleepPortraitWidth, kSleepPortraitHeight},
    {kSleepPanda, kSleepPandaWidth, kSleepPandaHeight},
};

}  // namespace

void setup() {
  shell::Config config;
  config.drawApp = drawApp;
  config.sleepApp = [] { player.powerDown(); };
  config.wakeApp = [] { player.powerUp(); };
  config.nowPlaying = [] { return musicPlaying() ? displayTitle(currentSong(), "Playing music") : String(); };
  config.beforePowerOff = [] { player.stop(); };
  config.offPictures = kOffPictures;
  config.offPictureCount = sizeof(kOffPictures) / sizeof(kOffPictures[0]);
  shell::begin(config);

  sdReady = storage::begin();
  if (sdReady) music::begin();
  Preferences prefs;
  prefs.begin("music", true);
  shuffle = prefs.getBool("shuffle", false);
  prefs.end();
  loadQueue("");
  buildOrder(false);
  LOGI("boot", "audio %s", player.begin() ? "ok" : "FAILED");
  player.setVolume(kVolumePercent);
  registerCommands();
  shell::showApp();
}

void loop() {
  // Music keeps the CPU awake, also while locked.
  power::setWakeLock(power::kWakeLockAudio, musicPlaying());

  shell::Input input;
  const bool unlocked = shell::update(input);
  transfer::poll();

  // A song we started ended by itself: go on with the next one.
  if (autoAdvance && !player.isPlaying()) {
    autoAdvance = false;
    if (orderPos + 1 < static_cast<int>(order.size())) next();
    stateDirty = true;
  }
  notifyState();
  if (!unlocked) return;

  if (transfer::active()) {
    loopTransfer();
    delay(10);
    return;
  }

  static bool redraw = false;
  static bool fullRequested = false;
  // The transfer result goes away by itself, or with a tap.
  if (resultUntil && (millis() > resultUntil || input.gesture == Touch::Gesture::Tap)) {
    resultUntil = 0;
    redraw = true;
    input.gesture = Touch::Gesture::None;
  }
  if (input.gesture == Touch::Gesture::Tap) redraw |= onTap(shell::touch.x(), shell::touch.y());
  if (input.gesture == Touch::Gesture::LongPress) fullRequested = true;  // clears ghosting
  if (input.boot && !queue.empty()) next();
  redraw |= stateDirty;
  stateDirty = false;

  // Input is handled above immediately; drawing waits until the panel is free.
  if (!epd.isBusy()) {
    loopPlayer(redraw, fullRequested);
    redraw = false;
    fullRequested = false;
  }
  delay(10);
}
