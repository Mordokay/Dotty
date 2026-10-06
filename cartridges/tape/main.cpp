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

DOTTY_CARTRIDGE("tape", "Tape Recorder", "0.4.6");

namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;
constexpr int16_t kNavH = nav::kHeight;
constexpr int16_t kNavButton = nav::kTouch;  // corner tap width
constexpr int16_t kRowH = 38;
constexpr int kMenuRows = (kW - kNavH - 2) / kRowH;
constexpr uint32_t kSavedMs = 3000;      // "Saved" under the cassette
constexpr uint32_t kBootHoldMs = 120;    // BOOT this long = record (a click is not)
constexpr int16_t kSaveTop = 164;        // the deck's Undo · Save · Discard row (to the bottom)
constexpr uint32_t kConfirmMs = 10000;   // "Discard this tape?" goes away by itself
constexpr int16_t kAskTop = 74;          // its Yes / No buttons (where the cassette was)
constexpr int16_t kAskDeleteTop = 136;   // the player's Yes / No (where the controls were)
constexpr int16_t kButtonY = 114;        // the player's play button
constexpr int16_t kVolumeY = 191;

AudioPlayer player;
bool sdReady = false;

enum class Screen { Deck, List, Player };
Screen screen = Screen::Deck;
int listPage = 0;
std::vector<tape::Recording> recordings;  // newest first
int playIndex = 0;                        // in recordings, for the player
String savedName;                         // the tape just saved, shown for a moment
uint32_t savedUntil = 0;
uint32_t confirmDiscardUntil = 0;         // the trash was tapped: "Discard this tape?" until then
uint32_t confirmDeleteUntil = 0;          // the player's trash: "Delete this recording?" until then
bool resumeAfterAsk = false;              // it was playing when asked: No plays on
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

void notifyList();

void saveTape() {
  savedName = tape::finish();
  savedUntil = savedName.length() ? millis() + kSavedMs : 0;
  listDirty = true;
  stateDirty = true;
  notifyList();  // the app's list shows the new recording
}

// Paused: the last part goes (the tape stays open, maybe empty).
void undoPart() {
  tape::undo();
  confirmDiscardUntil = 0;
  stateDirty = true;
}

void discardTape() {
  tape::discard();
  confirmDiscardUntil = 0;
  savedUntil = 0;
  stateDirty = true;
}

bool askingDiscard() {
  return confirmDiscardUntil && millis() < confirmDiscardUntil;
}

// The paused deck's buttons: undo arrow | Save | trash (x ranges, also used for taps).
constexpr int16_t kUndoX = 2, kUndoW = 46, kSaveX = 54, kSaveW = 92, kDiscardX = 152, kDiscardW = 46;

void button(int16_t x, int16_t y, int16_t w, int16_t h, const char *label, bool filled, bool enabled = true) {
  const int16_t r = h / 2;
  if (filled) {
    epd.fillRoundRect(x, y, w, h, r, kBlack);
  } else {
    epd.drawRoundRect(x, y, w, h, r, kBlack);
    if (enabled) epd.drawRoundRect(x + 1, y + 1, w - 2, h - 2, r - 1, kBlack);  // disabled: thin
  }
  if (!label) return;
  epd.setFont(&FreeSansBold9pt7b);
  epd.setTextColor(filled ? kWhite : kBlack);
  epd.setCursor(x + (w - ui::textWidth(epd, label)) / 2, y + h / 2 + 6);
  epd.print(label);
  epd.setTextColor(kBlack);
}

void thick(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  for (int d = 0; d < 2; d++) {
    epd.drawLine(x0 + d, y0, x1 + d, y1, kBlack);
    epd.drawLine(x0, y0 + d, x1, y1 + d, kBlack);
  }
}

// Undo: an arc over the top, its left end an arrow head pointing down.
void undoIcon(int16_t cx, int16_t cy) {
  const int16_t ax = cx + 1, ay = cy + 3, r = 8;
  for (int deg = 180; deg < 360; deg += 10) {
    const float a0 = deg * PI / 180, a1 = (deg + 10) * PI / 180;
    thick(ax + cosf(a0) * r, ay + sinf(a0) * r, ax + cosf(a1) * r, ay + sinf(a1) * r);
  }
  thick(ax + r, ay, ax + r, ay + 5);  // the tail on the right goes down a little
  epd.fillTriangle(ax - r - 5, ay - 1, ax - r + 5, ay - 1, ax - r, ay + 6, kBlack);
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
  const int16_t w = 104, h = 56, x = cx - w / 2;
  epd.fillRoundRect(x, top, w, h, 7, kBlack);
  epd.fillRoundRect(x + 4, top + 4, w - 8, h - 8, 4, kWhite);
  epd.drawRoundRect(x + 14, top + 13, w - 28, 26, 12, kBlack);  // window
  for (int side = -1; side <= 1; side += 2) {
    const int16_t rx = cx + side * 25, ry = top + 26;
    epd.fillCircle(rx, ry, 10, kBlack);
    epd.fillCircle(rx, ry, 4, kWhite);
    for (int k = 0; k < 3; k++) {
      const float a = (turning ? 0.5f : 0.0f) + k * 2.094f;
      epd.drawLine(rx, ry, rx + cosf(a) * 8, ry + sinf(a) * 8, kWhite);
    }
  }
  epd.fillRect(x + 26, top + h - 10, w - 52, 6, kBlack);  // the bottom notch
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
  if (s == tape::State::Paused && askingDiscard()) {
    // Where the cassette was: the question and Yes / No.
    epd.setFont(&FreeSansBold9pt7b);
    ui::drawCentered(epd, "Discard this tape?", kNavH + 20);
    button(12, kAskTop, 84, 30, "Yes", true);
    button(104, kAskTop, 84, 30, "No", false);
  } else {
    drawCassette(kW / 2, kNavH + 4, spokes);  // to y 105
  }

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
    // What Undo would take away, so it's never a surprise.
    const uint32_t last = tape::lastPartMs();
    ui::drawCentered(epd, tape::parts() > 0 ? "Undo takes the last " + clock(last) : String("Hold BOOT to go on"), 156);
    if (askingDiscard()) return;  // Yes / No above; the row comes back after
    const int16_t y = kSaveTop + 2;
    button(kUndoX, y, kUndoW, 30, nullptr, false, tape::parts() > 0);
    undoIcon(kUndoX + kUndoW / 2, y + 15);
    button(kSaveX, y, kSaveW, 30, "Save", true);
    button(kDiscardX, y, kDiscardW, 30, nullptr, false);
    nav::drawIcon(epd, nav::Icon::Trash, kDiscardX + kDiscardW / 2, y + 15, kBlack);
  }
}

// Every row holds a recording; the page ("1/2") sits in the nav bar's right corner.
int listPages(int count) {
  return max(1, (count + kMenuRows - 1) / kMenuRows);
}

void drawList() {
  epd.fillScreen(kWhite);
  const int per = kMenuRows;
  const int pages = listPages(recordings.size());
  listPage = constrain(listPage, 0, pages - 1);
  nav::draw(epd, "Tapes", nav::Icon::Back, nav::Icon::None, nav::pageLabel(listPage, pages));
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSans9pt7b);
  if (recordings.empty()) {
    ui::drawCentered(epd, "No recordings yet", 105);
    ui::drawCentered(epd, "Hold BOOT to record", 127);
    return;
  }
  for (int row = 0; row < per; row++) {
    const int i = listPage * per + row;
    if (i >= static_cast<int>(recordings.size())) break;
    const int16_t top = kNavH + 2 + row * kRowH;
    const String length = clock(recordings[i].durationMs);
    const int16_t lengthW = ui::textWidth(epd, length);
    epd.setCursor(10, top + kRowH / 2 + 6);
    epd.print(ui::fitText(epd, ui::printable(tape::displayName(recordings[i].name)), kW - 30 - lengthW));
    epd.setCursor(kW - 10 - lengthW, top + kRowH / 2 + 6);
    epd.print(length);
    epd.drawFastHLine(10, top + kRowH - 1, kW - 20, kBlack);
  }
}

bool askingDelete() {
  return confirmDeleteUntil && millis() < confirmDeleteUntil;
}

void drawPlayer() {
  epd.fillScreen(kWhite);
  const int count = recordings.size();
  nav::draw(epd, String(playIndex + 1) + " / " + String(count), nav::Icon::Back,
            count ? nav::Icon::Trash : nav::Icon::None);
  epd.setTextColor(kBlack);
  if (count == 0) return;
  const tape::Recording &r = recordings[playIndex];
  epd.setFont(&FreeSansBold9pt7b);
  ui::drawWrapped(epd, ui::printable(tape::displayName(r.name)), 64, kW - 16, 2, 18);
  if (askingDelete()) {  // in place of the controls
    ui::drawCentered(epd, "Delete this recording?", 124);
    button(12, kAskDeleteTop, 84, 30, "Yes", true);
    button(104, kAskDeleteTop, 84, 30, "No", false);
    return;
  }

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
  const int16_t barX = 15, barY = 148, barW = kW - 30, barH = 8;
  epd.drawRect(barX, barY, barW, barH, kBlack);
  if (dur > 0) epd.fillRect(barX + 2, barY + 2, static_cast<int64_t>(barW - 4) * min(pos, dur) / dur, barH - 4, kBlack);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, clock(pos) + " / " + clock(dur), 171);
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

// The player's recording goes; the next one shows (not playing), or the list if none is left.
void deleteCurrent() {
  confirmDeleteUntil = 0;
  if (recordings.empty()) return;
  player.stop();
  tape::remove(recordings[playIndex].name);
  reloadList();
  notifyList();
  if (recordings.empty()) {
    screen = Screen::List;
    return;
  }
  playIndex = min<int>(playIndex, recordings.size() - 1);
  stateDirty = true;
}

bool onTap(uint16_t x, uint16_t y) {
  if (screen == Screen::Deck) {
    if (y < kNavH && x > kW - kNavButton && tape::state() != tape::State::Recording) {
      screen = Screen::List;
      listPage = 0;
      return true;
    }
    if (tape::state() == tape::State::Paused && askingDiscard()) {
      if (y < kAskTop - 8 || y > kAskTop + 38) return false;
      if (x < kW / 2) discardTape();  // Yes
      else confirmDiscardUntil = 0;   // No
      return true;
    }
    if (tape::state() == tape::State::Paused && y >= kSaveTop) {
      if (x < kSaveX - 2) {
        if (tape::parts() == 0) return false;
        undoPart();
      } else if (x < kDiscardX - 2) {
        saveTape();
      } else {
        confirmDiscardUntil = millis() + kConfirmMs;  // ask first
      }
      return true;
    }
    return false;
  }
  if (screen == Screen::List) {
    if (y < kNavH) {
      if (x < kNavButton) {
        screen = Screen::Deck;
      } else if (x > kW - kNavButton && listPages(recordings.size()) > 1) {
        listPage = (listPage + 1) % listPages(recordings.size());  // the page number: next, round
      } else {
        return false;
      }
      return true;
    }
    const int per = kMenuRows;
    const int row = (y - kNavH - 2) / kRowH;
    if (row >= per) return false;
    const int i = listPage * per + row;
    if (row < 0 || i >= static_cast<int>(recordings.size())) return false;
    playAt(i);
    return true;
  }
  // Player
  if (askingDelete()) {
    if (y < kAskDeleteTop - 8 || y > kAskDeleteTop + 38) return false;
    if (x < kW / 2) {  // Yes
      deleteCurrent();
    } else {           // No
      confirmDeleteUntil = 0;
      if (resumeAfterAsk) player.togglePause();
    }
    return true;
  }
  if (y < kNavH) {
    if (x < kNavButton) {
      player.stop();
      screen = Screen::List;
      listPage = playIndex / kMenuRows;  // back where that recording is
    } else if (x > kW - kNavButton && !recordings.empty()) {
      resumeAfterAsk = playing();
      if (resumeAfterAsk) player.togglePause();
      confirmDeleteUntil = millis() + kConfirmMs;  // ask first
    } else {
      return false;
    }
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
  if (s == tape::State::Paused) {
    out["parts"] = tape::parts();
    out["lastPart"] = tape::lastPartMs() / 1000.0f;
  }
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
  // The paused tape, from the app: undo the last part, save, or throw it away.
  ble::on("tape.undo", [](JsonObjectConst, JsonObject reply) {
    if (tape::state() != tape::State::Paused || tape::parts() == 0) return fail(reply, "nothing to undo");
    reply["removed"] = tape::lastPartMs() / 1000.0f;
    undoPart();
    addState(reply);
  });
  ble::on("tape.save", [](JsonObjectConst, JsonObject reply) {
    if (tape::state() != tape::State::Paused) return fail(reply, "no paused tape");
    saveTape();
    reply["name"] = savedName;
    addState(reply);
  });
  ble::on("tape.discard", [](JsonObjectConst, JsonObject reply) {
    if (tape::state() == tape::State::Idle) return fail(reply, "no tape");
    discardTape();
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
  if (confirmDeleteUntil && millis() > confirmDeleteUntil) {  // unanswered: back to the controls
    confirmDeleteUntil = 0;
    if (resumeAfterAsk && player.isPaused()) player.togglePause();
    redraw = true;
  }
  if (askingDelete()) input.gesture = input.gesture == Touch::Gesture::Tap ? input.gesture : Touch::Gesture::None;
  if (screen == Screen::Player && input.gesture == Touch::Gesture::SwipeLeft) redraw |= (playAt(playIndex + 1), true);
  if (screen == Screen::Player && input.gesture == Touch::Gesture::SwipeRight) redraw |= (playAt(playIndex - 1), true);
  if (input.gesture == Touch::Gesture::LongPress) full = true;
  if (screen == Screen::List) {  // pages: swipe left/up = next, right/down = previous
    using G = Touch::Gesture;
    if (input.gesture == G::SwipeLeft || input.gesture == G::SwipeUp) listPage++, redraw = true;
    if (input.gesture == G::SwipeRight || input.gesture == G::SwipeDown) listPage--, redraw = true;
  }
  if (savedUntil && millis() > savedUntil) {
    savedUntil = 0;
    redraw = true;
  }
  // The question went unanswered, or BOOT took the tape on: back to the cassette.
  if (confirmDiscardUntil && (millis() > confirmDiscardUntil || tape::state() != tape::State::Paused)) {
    confirmDiscardUntil = 0;
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
