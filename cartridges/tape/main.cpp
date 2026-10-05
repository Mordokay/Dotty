// Tape Recorder: a pocket tape deck. Hold BOOT to record, let go to pause; the same tape
// continues each time until ■ Save. Recordings (WAV on the SD card, recorder.h) are listed,
// played on Dotty's speaker, and fetched by the app over Wi-Fi (transfer.*: download).
//
// Screens: the deck (cassette, REC / PAUSED with the time and a level meter, Save), the
// recordings list (paged), and the player (swipe for the next/previous recording).
// While recording Dotty stays on the deck and never auto-locks; paused, it can lock (the lock
// screen says so) and unlocking comes back to the deck.

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include <SD_MMC.h>

#include "audio_player.h"
#include "board_pins.h"
#include "cartridge.h"
#include "core_ble.h"
#include "images/sleep_panda.h"
#include "images/sleep_portrait.h"
#include "log.h"
#include "nav_bar.h"
#include "power.h"
#include "recorder.h"
#include "shell.h"
#include "storage.h"
#include "transfer.h"
#include "ui.h"

DOTTY_CARTRIDGE("tape", "Tape Recorder", "0.1.0");

namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;
constexpr int16_t kNavH = nav::kHeight;
constexpr int16_t kNavButton = nav::kButton;
constexpr int16_t kRowH = 28;
constexpr int kMenuRows = (kW - kNavH - 2) / kRowH;
constexpr uint32_t kSavedMs = 3000;      // "Saved" under the cassette
constexpr uint32_t kBootHoldMs = 120;    // BOOT this long = record (a click is not)
constexpr int16_t kSaveTop = 164;        // the deck's ■ Save button (to the bottom)
constexpr int16_t kButtonY = 112;        // the player's play button
constexpr int16_t kVolumeY = 186;

AudioPlayer player;
bool sdReady = false;

enum class Screen { Deck, List, Player };
Screen screen = Screen::Deck;
int listPage = 0;
std::vector<tape::Recording> recordings;  // newest first
int playIndex = 0;                        // in recordings, for the player
String savedName;                         // the tape just saved, shown for a moment
uint32_t savedUntil = 0;
bool stateDirty = true;
bool listDirty = true;

// ---------- state ----------

void reloadList() {
  recordings = tape::list();
  listDirty = false;
}

String clock(uint32_t ms) {
  return ui::formatDuration(ms);
}

bool playing() {
  return player.isPlaying() && !player.isPaused();
}

void startRecording() {
  if (player.isPlaying()) player.stop();
  if (!sdReady || !tape::record()) return;
  screen = Screen::Deck;
  savedUntil = 0;
  stateDirty = true;
}

void pauseRecording() {
  tape::pause();
  stateDirty = true;
}

void saveTape() {
  savedName = tape::finish();
  savedUntil = savedName.length() ? millis() + kSavedMs : 0;
  listDirty = true;
  stateDirty = true;
}

void playAt(int index) {
  if (recordings.empty()) return;
  const int count = recordings.size();
  playIndex = ((index % count) + count) % count;
  player.play(tape::path(recordings[playIndex].name).c_str());
  screen = Screen::Player;
  stateDirty = true;
}

// ---------- drawing ----------

// A cassette: shell, label window and two reels (turning = spokes at another angle).
void drawCassette(int16_t cx, int16_t top, bool turning) {
  const int16_t w = 116, h = 62, x = cx - w / 2;
  epd.fillRoundRect(x, top, w, h, 7, kBlack);
  epd.fillRoundRect(x + 4, top + 4, w - 8, h - 8, 4, kWhite);
  epd.drawRoundRect(x + 16, top + 15, w - 32, 28, 12, kBlack);  // window
  for (int side = -1; side <= 1; side += 2) {
    const int16_t rx = cx + side * 28, ry = top + 29;
    epd.fillCircle(rx, ry, 11, kBlack);
    epd.fillCircle(rx, ry, 4, kWhite);
    for (int k = 0; k < 3; k++) {
      const float a = (turning ? 0.5f : 0.0f) + k * 2.094f;
      epd.drawLine(rx, ry, rx + cosf(a) * 9, ry + sinf(a) * 9, kWhite);
    }
  }
  epd.fillRect(x + 28, top + h - 11, w - 56, 7, kBlack);  // the bottom notch
}

void drawDeck() {
  epd.fillScreen(kWhite);
  nav::draw(epd, "Recorder", nav::Icon::None, nav::Icon::Playlists);
  epd.setTextColor(kBlack);
  const tape::State s = tape::state();
  if (!sdReady) {
    epd.setFont(&FreeSansBold9pt7b);
    ui::drawCentered(epd, "No SD card", 110);
    return;
  }
  static bool spokes = false;
  spokes = s == tape::State::Recording ? !spokes : spokes;
  drawCassette(kW / 2, kNavH + 6, spokes);  // to y 98

  if (s == tape::State::Idle) {
    epd.setFont(&FreeSansBold9pt7b);
    if (savedUntil) {
      ui::drawCentered(epd, "Saved", 134);
      epd.setFont(&FreeSans9pt7b);
      ui::drawCentered(epd, tape::displayName(savedName), 158);
    } else {
      ui::drawCentered(epd, "Hold BOOT to record", 134);
      epd.setFont(&FreeSans9pt7b);
      const size_t n = recordings.size();
      ui::drawCentered(epd, n == 0 ? "No recordings yet" : n == 1 ? "1 recording" : String(n) + " recordings", 158);
    }
    return;
  }

  // REC 01:42 (dot + time) or PAUSED 01:42.
  epd.setFont(&FreeSansBold18pt7b);
  const String time = clock(tape::elapsedMs());
  const int16_t timeW = ui::textWidth(epd, time);
  if (s == tape::State::Recording) {
    const int16_t total = 18 + timeW, x = (kW - total) / 2;
    epd.fillCircle(x + 6, 122, 7, kBlack);
    epd.setCursor(x + 18, 134);
    epd.print(time);
    // Level meter: 20 bars.
    const int bars = tape::level() / 5;
    for (int i = 0; i < 20; i++) {
      const int16_t bx = 20 + i * 8;
      if (i < bars) epd.fillRect(bx, 146, 6, 14, kBlack);
      else epd.drawFastHLine(bx, 159, 6, kBlack);
    }
    epd.setFont(&FreeSans9pt7b);
    ui::drawCentered(epd, "Let go of BOOT to pause", 188);
  } else {
    epd.setCursor((kW - timeW) / 2, 134);
    epd.print(time);
    epd.setFont(&FreeSans9pt7b);
    ui::drawCentered(epd, "Hold BOOT to go on", 156);
    // ■ Save
    epd.fillRoundRect(44, kSaveTop + 2, kW - 88, 30, 15, kBlack);
    epd.fillRect(72, kSaveTop + 12, 10, 10, kWhite);
    epd.setTextColor(kWhite);
    epd.setFont(&FreeSansBold9pt7b);
    epd.setCursor(90, kSaveTop + 23);
    epd.print("Save");
    epd.setTextColor(kBlack);
  }
}

int perPage(int count) {
  return count <= kMenuRows ? kMenuRows : kMenuRows - 1;
}

void drawList() {
  epd.fillScreen(kWhite);
  nav::draw(epd, "Recordings", nav::Icon::Back, nav::Icon::None);
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSans9pt7b);
  if (recordings.empty()) {
    ui::drawCentered(epd, "No recordings yet", 105);
    ui::drawCentered(epd, "Hold BOOT to record", 127);
    return;
  }
  const int per = perPage(recordings.size());
  const int pages = (recordings.size() + per - 1) / per;
  listPage = constrain(listPage, 0, pages - 1);
  for (int row = 0; row < per; row++) {
    const int i = listPage * per + row;
    if (i >= static_cast<int>(recordings.size())) break;
    const int16_t top = kNavH + 2 + row * kRowH;
    const String length = clock(recordings[i].durationMs);
    const int16_t lengthW = ui::textWidth(epd, length);
    epd.setCursor(10, top + 19);
    epd.print(ui::fitText(epd, ui::printable(tape::displayName(recordings[i].name)), kW - 30 - lengthW));
    epd.setCursor(kW - 10 - lengthW, top + 19);
    epd.print(length);
    epd.drawFastHLine(10, top + kRowH - 1, kW - 20, kBlack);
  }
  if (pages > 1) {
    const int16_t top = kNavH + 2 + per * kRowH;
    ui::drawCentered(epd, String(listPage + 1) + " / " + String(pages), top + 19);
    if (listPage > 0) epd.fillTriangle(22, top + 13, 32, top + 6, 32, top + 20, kBlack);
    if (listPage < pages - 1) epd.fillTriangle(kW - 22, top + 13, kW - 32, top + 6, kW - 32, top + 20, kBlack);
  }
}

void drawPlayer() {
  epd.fillScreen(kWhite);
  const int count = recordings.size();
  nav::draw(epd, String(playIndex + 1) + " / " + String(count), nav::Icon::Back, nav::Icon::None);
  epd.setTextColor(kBlack);
  if (count == 0) return;
  const tape::Recording &r = recordings[playIndex];
  epd.setFont(&FreeSansBold9pt7b);
  ui::drawWrapped(epd, ui::printable(tape::displayName(r.name)), 54, kW - 16, 2, 18);

  const int16_t cx = kW / 2, cy = kButtonY;
  epd.fillCircle(cx, cy, 24, kBlack);
  if (playing()) {
    epd.fillRect(cx - 9, cy - 10, 6, 20, kWhite);
    epd.fillRect(cx + 3, cy - 10, 6, 20, kWhite);
  } else {
    epd.fillTriangle(cx - 6, cy - 12, cx - 6, cy + 12, cx + 12, cy, kWhite);
  }
  // Previous / next (also by swiping).
  epd.fillTriangle(38, cy, 50, cy - 9, 50, cy + 9, kBlack);
  epd.fillTriangle(kW - 38, cy, kW - 50, cy - 9, kW - 50, cy + 9, kBlack);

  const uint32_t pos = player.isPlaying() ? player.positionMs() : 0;
  const uint32_t dur = r.durationMs;
  const int16_t barX = 15, barY = 144, barW = kW - 30, barH = 8;
  epd.drawRect(barX, barY, barW, barH, kBlack);
  if (dur > 0) epd.fillRect(barX + 2, barY + 2, static_cast<int64_t>(barW - 4) * min(pos, dur) / dur, barH - 4, kBlack);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, clock(pos) + " / " + clock(dur), 168);
  // Volume:  -   vol 80%   +
  epd.fillRect(16, kVolumeY - 6, 14, 3, kBlack);
  epd.fillRect(kW - 30, kVolumeY - 6, 14, 3, kBlack);
  epd.fillRect(kW - 24, kVolumeY - 12, 3, 14, kBlack);
  ui::drawCentered(epd, "vol " + String(player.volume()) + "%", kVolumeY);
}

void drawTransfer() {
  const transfer::Status s = transfer::status();
  epd.fillScreen(kWhite);
  ui::drawHeader(epd, "Sending to iPhone");
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSansBold9pt7b);
  ui::drawCentered(epd, s.file.length() ? tape::displayName(s.file) : String("Waiting for the app"), 80);
  const int16_t barX = 15, barY = 105, barW = kW - 30, barH = 10;
  epd.drawRect(barX, barY, barW, barH, kBlack);
  if (s.total > 0) epd.fillRect(barX + 2, barY + 2, static_cast<int64_t>(barW - 4) * s.done / s.total, barH - 4, kBlack);
}

void drawApp() {
  if (listDirty) reloadList();
  if (transfer::active()) drawTransfer();
  else if (screen == Screen::List) drawList();
  else if (screen == Screen::Player) drawPlayer();
  else drawDeck();
}

// ---------- touch ----------

bool onTap(uint16_t x, uint16_t y) {
  if (screen == Screen::Deck) {
    if (y < kNavH && x > kW - kNavButton && tape::state() != tape::State::Recording) {
      screen = Screen::List;
      listPage = 0;
      return true;
    }
    if (tape::state() == tape::State::Paused && y >= kSaveTop) {
      saveTape();
      return true;
    }
    return false;
  }
  if (screen == Screen::List) {
    if (y < kNavH) {
      if (x >= kNavButton) return false;
      screen = Screen::Deck;
      return true;
    }
    const int per = perPage(recordings.size());
    const int row = (y - kNavH - 2) / kRowH;
    if (row >= per) {
      if (x < 70) listPage--;
      else if (x > kW - 70) listPage++;
      else return false;
      return true;
    }
    const int i = listPage * per + row;
    if (row < 0 || i >= static_cast<int>(recordings.size())) return false;
    playAt(i);
    return true;
  }
  // Player
  if (y < kNavH) {
    if (x >= kNavButton) return false;
    player.stop();
    screen = Screen::List;
    return true;
  }
  if (y > kVolumeY - 24) {
    if (x < 60) player.setVolume(player.volume() - 10);
    else if (x > kW - 60) player.setVolume(player.volume() + 10);
    else return false;
    return true;
  }
  if (y < kButtonY - 35 || y > kButtonY + 35) return false;
  if (x < 70) playAt(playIndex - 1);
  else if (x > kW - 70) playAt(playIndex + 1);
  else if (player.isPlaying()) player.togglePause();
  else playAt(playIndex);
  return true;
}

// ---------- app (BLE) ----------

void addState(JsonObject out) {
  const tape::State s = tape::state();
  out["state"] = s == tape::State::Recording ? "recording" : s == tape::State::Paused ? "paused" : "idle";
  out["elapsed"] = tape::elapsedMs() / 1000;
  if (player.isPlaying() && !recordings.empty()) {
    out["playing"] = recordings[playIndex].name;
    out["paused"] = player.isPaused();
    out["position"] = player.positionMs() / 1000;
  }
  out["volume"] = player.volume();
}

void notifyState() {
  JsonDocument event;
  event["event"] = "tape.state";
  addState(event.as<JsonObject>());
  ble::notify(event);
}

void notifyList() {
  JsonDocument event;
  event["event"] = "tape.list";
  ble::notify(event);
}

void fail(JsonObject reply, const char *error) {
  reply["ok"] = false;
  reply["error"] = error;
}

void registerCommands() {
  ble::on("tape.status", [](JsonObjectConst, JsonObject reply) { addState(reply); });
  ble::on("tape.list", [](JsonObjectConst, JsonObject reply) {
    if (listDirty) reloadList();
    JsonArray list = reply["recordings"].to<JsonArray>();
    for (const tape::Recording &r : recordings) {
      JsonObject o = list.add<JsonObject>();
      o["name"] = r.name;
      o["title"] = tape::displayName(r.name);
      o["size"] = r.size;
      o["duration"] = r.durationMs / 1000.0f;
      o["added"] = static_cast<int64_t>(r.added);  // local time as epoch seconds
    }
    reply["rate"] = tape::kRate;
  });
  ble::on("tape.play", [](JsonObjectConst args, JsonObject reply) {
    if (tape::state() == tape::State::Recording) return fail(reply, "recording");
    if (listDirty) reloadList();
    const String name = args["name"] | "";
    const auto it = std::find_if(recordings.begin(), recordings.end(), [&](const tape::Recording &r) { return r.name == name; });
    if (it == recordings.end()) return fail(reply, "no such recording");
    shell::wake();
    playAt(it - recordings.begin());
    addState(reply);
  });
  ble::on("tape.toggle", [](JsonObjectConst, JsonObject reply) {
    if (player.isPlaying()) player.togglePause();
    stateDirty = true;
    addState(reply);
  });
  ble::on("tape.stop", [](JsonObjectConst, JsonObject reply) {
    player.stop();
    stateDirty = true;
    addState(reply);
  });
  ble::on("tape.volume", [](JsonObjectConst args, JsonObject reply) {
    const int value = args["value"] | -1;
    if (value < 0 || value > 100) return fail(reply, "value must be 0-100");
    player.setVolume(value);
    stateDirty = true;
    addState(reply);
  });
  ble::on("tape.rename", [](JsonObjectConst args, JsonObject reply) {
    if (!tape::rename(args["name"] | "", args["to"] | "")) return fail(reply, "that name is empty or taken");
    listDirty = true;
    stateDirty = true;
    notifyList();
  });
  ble::on("tape.delete", [](JsonObjectConst args, JsonObject reply) {
    const String name = args["name"] | "";
    if (player.isPlaying() && !recordings.empty() && recordings[playIndex].name == name) player.stop();
    if (!tape::remove(name)) return fail(reply, "no such recording");
    listDirty = true;
    if (screen == Screen::Player) screen = Screen::List;
    stateDirty = true;
    notifyList();
  });
  // Recordings go to the phone with GET /download?dir=recordings&name=…
  transfer::registerCommands([](const transfer::Summary &) { shell::showApp(); });
}

// ---------- developer aid: a recording over USB ----------

// 'w': the newest recording as hex lines between "#WAV <bytes>" and "#END" (only while a
// computer has the port open), for checking the sound on the Mac.
void sendNewestOverSerial() {
  if (!HWCDC::isPlugged()) return;
  if (listDirty) reloadList();
  if (recordings.empty()) return;
  File f = SD_MMC.open(tape::path(recordings.front().name));
  if (!f) return;
  Serial.printf("\n#WAV %u %s\n", static_cast<unsigned>(f.size()), recordings.front().name.c_str());
  uint8_t buf[50];
  char line[2 * sizeof(buf) + 2];
  for (size_t n; (n = f.read(buf, sizeof(buf))) > 0;) {
    size_t len = 0;
    for (size_t j = 0; j < n; j++) len += snprintf(line + len, sizeof(line) - len, "%02x", buf[j]);
    line[len++] = '\n';
    Serial.write(reinterpret_cast<uint8_t *>(line), len);
  }
  Serial.flush();
  Serial.print("#END\n");
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
  config.nowPlaying = []() -> String {
    if (tape::state() == tape::State::Paused) return "Tape paused " + clock(tape::elapsedMs());
    if (playing() && !recordings.empty()) return "Playing " + ui::printable(tape::displayName(recordings[playIndex].name));
    return String();
  };
  config.beforePowerOff = [] {
    tape::finish();  // keep what was recorded
    player.stop();
  };
  config.offPictures = kOffPictures;
  config.offPictureCount = sizeof(kOffPictures) / sizeof(kOffPictures[0]);
  shell::begin(config);

  sdReady = storage::begin();
  LOGI("boot", "audio %s", player.begin(tape::kRate) ? "ok" : "FAILED");
  player.setVolume(80);
  if (sdReady && !tape::begin(player)) LOGE("tape", "recorder setup failed");
  reloadList();
  registerCommands();
  shell::showApp();
}

void loop() {
  const tape::State s = tape::state();
  power::setWakeLock(power::kWakeLockAudio, s == tape::State::Recording || playing());

  shell::Input input;
  const bool unlocked = shell::update(input);
  transfer::poll();

  static bool wasLocked = false;
  if (!unlocked) {
    if (s == tape::State::Recording) pauseRecording();  // locked mid-recording: keep it, paused
    wasLocked = true;
    return;
  }
  if (wasLocked) {  // unlocked: an open tape brings the deck back, to save or go on
    wasLocked = false;
    if (tape::state() != tape::State::Idle && screen != Screen::Deck) {
      screen = Screen::Deck;
      stateDirty = true;
    }
  }
  if (transfer::active()) {
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw > 1000 && !epd.isBusy()) {
      lastDraw = millis();
      shell::wake();
      drawTransfer();
      shell::refresh(false);
    }
    delay(10);
    return;
  }

  // BOOT held = record, released = pause (a short click does nothing).
  static uint32_t bootDownAt = 0;
  static bool devHold = false;  // developer aid: 'r' holds BOOT down until pressed again
  if (input.key == 'r') devHold = !devHold;
  if (input.key == 'v' && tape::state() == tape::State::Paused) saveTape();
  const bool bootDown = digitalRead(PIN_BTN_BOOT) == LOW || devHold;
  if (bootDown && !bootDownAt) bootDownAt = millis();
  if (!bootDown) bootDownAt = 0;
  const bool holding = bootDownAt && millis() - bootDownAt >= kBootHoldMs;
  if (holding && s != tape::State::Recording) startRecording();
  if (!bootDown && s == tape::State::Recording) pauseRecording();
  if (tape::state() == tape::State::Recording) shell::wake();  // never auto-lock mid-recording

  if (input.key == 'w') sendNewestOverSerial();
  if (input.key == 'l') shell::lock();
  static bool redraw = false, full = false;
  if (input.gesture == Touch::Gesture::Tap) redraw |= onTap(shell::touch.x(), shell::touch.y());
  if (screen == Screen::Player && input.gesture == Touch::Gesture::SwipeLeft) redraw |= (playAt(playIndex + 1), true);
  if (screen == Screen::Player && input.gesture == Touch::Gesture::SwipeRight) redraw |= (playAt(playIndex - 1), true);
  if (input.gesture == Touch::Gesture::LongPress) full = true;
  if (savedUntil && millis() > savedUntil) {
    savedUntil = 0;
    redraw = true;
  }

  // The time and meter while recording, the position while playing: once a second.
  static uint32_t lastTick = 0;
  const bool ticking = (screen == Screen::Deck && tape::state() == tape::State::Recording) ||
                       (screen == Screen::Player && playing());
  if (ticking && millis() - lastTick >= 1000) redraw = true;
  static bool wasPlaying = false;
  if (wasPlaying != player.isPlaying()) {
    wasPlaying = player.isPlaying();
    stateDirty = true;
  }
  if (stateDirty) {
    stateDirty = false;
    redraw = true;
    notifyState();
  }
  if ((redraw || full) && !epd.isBusy()) {
    lastTick = millis();
    drawApp();
    shell::refresh(full);
    redraw = full = false;
  }
  delay(10);
}
