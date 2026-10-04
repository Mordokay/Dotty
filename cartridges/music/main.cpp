// Music cartridge: plays the first MP3 in /music on the SD card, with play/pause and
// volume on screen. Also hosts the e-paper refresh test (BOOT toggles it).

#include <Arduino.h>
#include <FS.h>
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
#include "power.h"
#include "shell.h"
#include "ui.h"

DOTTY_CARTRIDGE("music", "Music", "0.5.0");

namespace {

using shell::epd;

constexpr uint32_t kPlayerRefreshMs = 1000;
constexpr uint8_t kVolumePercent = 80;
constexpr uint8_t kVolumeStep = 10;
constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

constexpr int16_t kButtonY = 100;
constexpr int16_t kVolDownX = 32;
constexpr int16_t kVolUpX = kW - 32;

enum class Screen { Player, RefreshTest };

AudioPlayer player;
Screen screen = Screen::Player;
bool sdReady = false;
String songPath;
String songTitle;
uint32_t lastRefreshMs = 0;
bool redrawRequested = false;  // set by BLE commands that change what the player shows

// "/music/NAPA-Deslocado.mp3" -> "NAPA - Deslocado"
String titleFromPath(const String &path) {
  String name = path.substring(path.lastIndexOf('/') + 1);
  const int dot = name.lastIndexOf('.');
  if (dot > 0) name.remove(dot);
  name.replace("_", " ");
  name.replace("-", " - ");
  return name;
}

bool musicPlaying() {
  return player.isPlaying() && !player.isPaused();
}

// ---------- player screen ----------

void drawPlayer() {
  epd.fillScreen(kWhite);
  ui::drawHeader(epd, "Music");

  epd.setFont(&FreeSansBold9pt7b);
  if (!sdReady) {
    ui::drawCentered(epd, "No SD card", 105);
    return;
  }
  if (songPath.isEmpty()) {
    ui::drawCentered(epd, "No MP3 in /music", 105);
    return;
  }
  ui::drawCentered(epd, ui::fitText(epd, songTitle, kW - 12), 52);

  // Play/pause button (shows the action a tap will perform), flanked by volume - / +.
  const int16_t cx = kW / 2, cy = kButtonY;
  epd.fillCircle(cx, cy, 30, kBlack);
  for (int16_t bx : {kVolDownX, kVolUpX}) {
    epd.drawCircle(bx, cy, 18, kBlack);
    epd.drawCircle(bx, cy, 17, kBlack);
    epd.fillRect(bx - 8, cy - 1, 17, 3, kBlack);
  }
  epd.fillRect(kVolUpX - 1, cy - 8, 3, 17, kBlack);
  if (musicPlaying()) {
    epd.fillRect(cx - 11, cy - 13, 8, 26, kWhite);
    epd.fillRect(cx + 3, cy - 13, 8, 26, kWhite);
  } else {
    epd.fillTriangle(cx - 8, cy - 15, cx - 8, cy + 15, cx + 15, cy, kWhite);
  }

  // Volume, progress bar + time.
  const uint32_t pos = player.isPlaying() ? player.positionMs() : 0;
  const uint32_t dur = player.durationMs();
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, "vol " + String(player.volume()) + "%", 150);
  const int16_t barX = 15, barY = 160, barW = kW - 30, barH = 8;
  epd.drawRect(barX, barY, barW, barH, kBlack);
  if (dur > 0) {
    const int16_t fill = static_cast<int64_t>(barW - 4) * min(pos, dur) / dur;
    epd.fillRect(barX + 2, barY + 2, fill, barH - 4, kBlack);
  }
  ui::drawCentered(epd, ui::formatDuration(pos) + " / " + ui::formatDuration(dur), 192);
}

// Returns true when the screen needs a redraw.
bool onPlayerTap(uint16_t x, uint16_t y) {
  if (!sdReady || songPath.isEmpty()) return false;
  const bool buttonRow = y > kButtonY - 35 && y < kButtonY + 35;
  if (buttonRow && (x < kVolDownX + 30 || x > kVolUpX - 30)) {
    const int step = x < kW / 2 ? -kVolumeStep : kVolumeStep;
    player.setVolume(constrain(player.volume() + step, 0, 100));
    LOGI("player", "volume %u%%", player.volume());
    return true;
  }
  if (player.isPlaying()) {
    player.togglePause();
  } else {
    player.play(songPath.c_str());
  }
  LOGI("player", "%s", !player.isPlaying() ? "stopped" : player.isPaused() ? "paused" : "playing");
  return false;  // play state changes are picked up by the loop
}

// ---------- refresh test screen ----------

// Back-to-back partial refreshes of moving content, never a full refresh unless
// tapped. Watch for leftover "shadows" and note the counter when they appear.
void drawRefreshTest(int frame, int partials, uint32_t lastMs) {
  epd.fillScreen(kWhite);
  ui::drawHeader(epd, "Refresh test");

  epd.setFont(&FreeSansBold18pt7b);
  ui::drawCentered(epd, String(partials), 70);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, "partials since full", 92);

  // Bouncing square across a lane, plus a block that inverts every frame.
  const int16_t lane = kW - 30 - 40;
  int16_t pos = (frame * 17) % (2 * lane);
  if (pos > lane) pos = 2 * lane - pos;
  epd.fillRect(10 + pos, 104, 30, 30, kBlack);
  epd.fillRect(kW - 34, 104, 24, 30, frame % 2 ? kBlack : kWhite);
  epd.drawRect(kW - 34, 104, 24, 30, kBlack);

  char line[32];
  snprintf(line, sizeof(line), "%lu ms  (%.1f fps)", lastMs, lastMs ? 1000.0f / lastMs : 0.0f);
  ui::drawCentered(epd, line, 160);
  ui::drawCentered(epd, "tap = full refresh", 188);
}

void loopRefreshTest(bool fullRequested) {
  static int frame = 0;
  drawRefreshTest(++frame, fullRequested ? 0 : shell::partialsSinceFull() + 1, lastRefreshMs);
  const uint32_t start = millis();
  shell::refresh(fullRequested, false);
  epd.waitBusy();  // the test measures the panel's real update time
  lastRefreshMs = millis() - start;
  LOGI("refresh", "%s #%d, %lu ms", shell::partialsSinceFull() ? "partial" : "FULL",
       shell::partialsSinceFull(), lastRefreshMs);
}

// Redraws on any play state change, and once a second while playing.
void loopPlayer(bool redraw, bool fullRequested) {
  static uint32_t lastDraw = 0;
  static uint32_t lastPositionS = UINT32_MAX;
  static bool lastPlaying = false, lastPaused = false;

  const bool playing = player.isPlaying(), paused = player.isPaused();
  const uint32_t positionS = player.positionMs() / 1000;
  const bool stateChanged = playing != lastPlaying || paused != lastPaused;
  const bool tick = playing && !paused && positionS != lastPositionS &&
                    millis() - lastDraw >= kPlayerRefreshMs;
  if (fullRequested || redraw || stateChanged || tick) {
    lastPlaying = playing;
    lastPaused = paused;
    lastPositionS = positionS;
    lastDraw = millis();
    drawPlayer();
    shell::refresh(fullRequested);
  }
}

// ---------- BLE commands ----------

void addStatus(JsonObject reply) {
  reply["title"] = songTitle;
  reply["playing"] = musicPlaying();
  reply["paused"] = player.isPaused();
  reply["position"] = player.isPlaying() ? player.positionMs() / 1000 : 0;
  reply["duration"] = player.durationMs() / 1000;
  reply["volume"] = player.volume();
}

void registerCommands() {
  ble::on("music.status", [](JsonObjectConst, JsonObject reply) { addStatus(reply); });
  ble::on("music.toggle", [](JsonObjectConst, JsonObject reply) {
    if (!sdReady || songPath.isEmpty()) {
      reply["ok"] = false;
      reply["error"] = "no song";
      return;
    }
    player.isPlaying() ? player.togglePause() : (void)player.play(songPath.c_str());
    addStatus(reply);
  });
  ble::on("music.volume", [](JsonObjectConst args, JsonObject reply) {
    const int value = args["value"] | -1;
    if (value < 0 || value > 100) {
      reply["ok"] = false;
      reply["error"] = "value must be 0-100";
      return;
    }
    player.setVolume(value);
    redrawRequested = true;
    addStatus(reply);
  });
}

// ---------- shell hooks ----------

void drawApp() {
  if (screen == Screen::Player) {
    drawPlayer();
  } else {
    drawRefreshTest(0, 0, 0);
  }
}

String nowPlaying() {
  return musicPlaying() ? songTitle : String();
}

const shell::Picture kOffPictures[] = {
    {kSleepPortrait, kSleepPortraitWidth, kSleepPortraitHeight},
    {kSleepPanda, kSleepPandaWidth, kSleepPandaHeight},
};

// ---------- startup ----------

String findFirstMp3(const char *dirPath) {
  File dir = SD_MMC.open(dirPath);
  if (!dir || !dir.isDirectory()) return "";
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String name = f.name();
    String lower = name;
    lower.toLowerCase();
    if (!f.isDirectory() && !name.startsWith(".") && lower.endsWith(".mp3")) {
      return String(dirPath) + "/" + name;
    }
  }
  return "";
}

void initStorage() {
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  sdReady = SD_MMC.begin("/sdcard", true);  // 1-bit bus
  if (!sdReady) {
    LOGE("sd", "mount failed");
    return;
  }
  LOGI("sd", "%llu MB card, %llu MB used", SD_MMC.cardSize() >> 20, SD_MMC.usedBytes() >> 20);
  songPath = findFirstMp3("/music");
  songTitle = titleFromPath(songPath);
  LOGI("sd", "song '%s'", songPath.c_str());
}

}  // namespace

void setup() {
  shell::Config config;
  config.drawApp = drawApp;
  config.sleepApp = [] { player.powerDown(); };
  config.wakeApp = [] { player.powerUp(); };
  config.nowPlaying = nowPlaying;
  config.beforePowerOff = [] { player.stop(); };
  config.offPictures = kOffPictures;
  config.offPictureCount = sizeof(kOffPictures) / sizeof(kOffPictures[0]);
  shell::begin(config);

  initStorage();
  LOGI("boot", "audio %s", player.begin() ? "ok" : "FAILED");
  player.setVolume(kVolumePercent);
  registerCommands();
  shell::showApp();
}

void loop() {
  // Music keeps the CPU awake (also while locked) so playback never stops.
  power::setWakeLock(power::kWakeLockAudio, musicPlaying());

  shell::Input input;
  if (!shell::update(input)) return;

  const bool tapped = input.gesture == Touch::Gesture::Tap;
  const bool longPress = input.gesture == Touch::Gesture::LongPress;

  if (input.boot) {
    screen = screen == Screen::Player ? Screen::RefreshTest : Screen::Player;
    LOGI("ui", "screen: %s", screen == Screen::Player ? "player" : "refresh test");
    shell::showApp();
    return;
  }

  if (screen == Screen::RefreshTest) {
    loopRefreshTest(tapped || longPress);
    return;
  }

  static bool redraw = false;
  static bool fullRequested = false;
  if (tapped) redraw |= onPlayerTap(shell::touch.x(), shell::touch.y());
  redraw |= redrawRequested;
  redrawRequested = false;
  if (longPress) fullRequested = true;  // long press: full refresh to clear ghosting

  // Input is handled above immediately; drawing waits until the panel is free.
  if (!epd.isBusy()) {
    loopPlayer(redraw, fullRequested);
    redraw = false;
    fullRequested = false;
  }
  delay(10);
}
