// Music cartridge: a small music player for the SD card. Songs live in the cartridge's
// library, playlists are lists of library songs (see music_library.h). The phone manages
// the library over BLE and uploads songs over Wi-Fi (transfer.*).

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include <algorithm>

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

DOTTY_CARTRIDGE("music", "Music", "0.7.0");

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

void playIndex(int index) {
  if (queue.empty()) return;
  const int count = queue.size();
  queueIndex = ((index % count) + count) % count;
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

void next() {
  playIndex(queueIndex + 1);
}

void previous() {
  if (player.isPlaying() && player.positionMs() > kRestartThresholdMs) {
    playIndex(queueIndex);
  } else {
    playIndex(queueIndex - 1);
  }
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
  stateDirty = true;
}

// ---------- screens ----------

void drawTriangle(int16_t cx, int16_t cy, int16_t dir, uint16_t color) {
  epd.fillTriangle(cx - 8 * dir, cy - 10, cx - 8 * dir, cy + 10, cx + 7 * dir, cy, color);
}

void drawPlayer() {
  epd.fillScreen(kWhite);
  ui::drawHeader(epd, queueName.length() ? queueName.c_str() : "Music");

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
  ui::drawCentered(epd, ui::fitText(epd, music::title(currentSong()), kW - 12), 50);
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
  const String file = s.file.length() ? music::title(s.file) : String("Waiting for songs");
  ui::drawCentered(epd, ui::fitText(epd, file, kW - 12), 80);
  const int16_t barX = 15, barY = 105, barW = kW - 30, barH = 10;
  epd.drawRect(barX, barY, barW, barH, kBlack);
  if (s.total > 0) {
    epd.fillRect(barX + 2, barY + 2, static_cast<int64_t>(barW - 4) * s.done / s.total, barH - 4, kBlack);
  }
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, String(s.filesReceived) + (s.filesReceived == 1 ? " song received" : " songs received"), 150);
  ui::drawCentered(epd, "over Wi-Fi", 172);
}

void drawApp() {
  transfer::active() ? drawTransfer() : drawPlayer();
}

// ---------- touch ----------

// Returns true when the screen needs a redraw.
bool onTap(uint16_t x, uint16_t y) {
  if (!sdReady) return false;
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
  ble::on("music.playlist.add", [](JsonObjectConst args, JsonObject reply) {
    if (!music::addToPlaylist(args["name"] | "", stringList(args["songs"]))) return fail(reply, "no such playlist");
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
  transfer::registerCommands([] {
    music::rescan();
    refreshQueue();
    notifyLibraryChanged();
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
  const bool tick = playing && !paused && positionS != lastPositionS && millis() - lastDraw >= kPlayerRefreshMs;
  if (fullRequested || redraw || changed || tick) {
    lastPlaying = playing;
    lastPaused = paused;
    lastPositionS = positionS;
    lastSong = currentSong();
    lastDraw = millis();
    drawPlayer();
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
  config.nowPlaying = [] { return musicPlaying() ? music::title(currentSong()) : String(); };
  config.beforePowerOff = [] { player.stop(); };
  config.offPictures = kOffPictures;
  config.offPictureCount = sizeof(kOffPictures) / sizeof(kOffPictures[0]);
  shell::begin(config);

  sdReady = storage::begin();
  if (sdReady) music::begin();
  loadQueue("");
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
    if (queueIndex + 1 < static_cast<int>(queue.size())) next();
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
