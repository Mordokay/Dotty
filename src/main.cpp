#include <Arduino.h>
#include <FS.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <SD_MMC.h>
#include <Wire.h>

#include "audio_player.h"
#include "board_pins.h"
#include "epd_display.h"
#include "touch.h"

namespace {

constexpr uint32_t kPowerOffHoldMs = 2000;
constexpr uint32_t kPlayerRefreshMs = 1000;
constexpr int kPartialRefreshesPerFull = 50;
constexpr uint8_t kVolumePercent = 80;
constexpr uint8_t kVolumeStep = 10;
constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

constexpr int16_t kButtonY = 100;
constexpr int16_t kVolDownX = 32;
constexpr int16_t kVolUpX = kW - 32;

enum class Screen { Player, RefreshTest };

EpdDisplay epd;
AudioPlayer player;
Touch touch;

Screen screen = Screen::Player;
bool sdReady = false;
String songPath;
String songTitle;
int partialsSinceFull = 0;
uint32_t lastRefreshMs = 0;
volatile bool bootPressed = false;

void IRAM_ATTR onBootButton() {
  bootPressed = true;
}

// ---------- drawing helpers ----------

int16_t textWidth(const String &text) {
  int16_t x1, y1;
  uint16_t w, h;
  epd.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);
  return w;
}

void drawCentered(const String &text, int16_t y) {
  int16_t x1, y1;
  uint16_t w, h;
  epd.getTextBounds(text.c_str(), 0, y, &x1, &y1, &w, &h);
  epd.setCursor((kW - w) / 2 - x1, y);
  epd.print(text);
}

// Shortens text with "..." until it fits the given width.
String fitText(String text, int16_t maxWidth) {
  if (textWidth(text) <= maxWidth) return text;
  while (text.length() > 1 && textWidth(text + "...") > maxWidth) {
    text.remove(text.length() - 1);
  }
  return text + "...";
}

void drawHeader(const char *label) {
  epd.fillRect(0, 0, kW, 26, kBlack);
  epd.setFont(&FreeSans9pt7b);
  epd.setTextColor(kWhite);
  drawCentered(label, 18);
  epd.setTextColor(kBlack);
}

String formatTime(uint32_t ms) {
  char buf[12];
  const uint32_t s = ms / 1000;
  snprintf(buf, sizeof(buf), "%lu:%02lu", s / 60, s % 60);
  return buf;
}

// "/music/NAPA-Deslocado.mp3" -> "NAPA - Deslocado"
String titleFromPath(const String &path) {
  String name = path.substring(path.lastIndexOf('/') + 1);
  const int dot = name.lastIndexOf('.');
  if (dot > 0) name.remove(dot);
  name.replace("_", " ");
  name.replace("-", " - ");
  return name;
}

// ---------- player screen ----------

void drawPlayer() {
  epd.fillScreen(kWhite);
  drawHeader("Music");

  epd.setFont(&FreeSansBold9pt7b);
  if (!sdReady) {
    drawCentered("No SD card", 105);
    return;
  }
  if (songPath.isEmpty()) {
    drawCentered("No MP3 in /music", 105);
    return;
  }
  drawCentered(fitText(songTitle, kW - 12), 52);

  // Play/pause button (shows the action a tap will perform), flanked by volume - / +.
  const int16_t cx = kW / 2, cy = kButtonY;
  epd.fillCircle(cx, cy, 30, kBlack);
  for (int16_t bx : {kVolDownX, kVolUpX}) {
    epd.drawCircle(bx, cy, 18, kBlack);
    epd.drawCircle(bx, cy, 17, kBlack);
    epd.fillRect(bx - 8, cy - 1, 17, 3, kBlack);
  }
  epd.fillRect(kVolUpX - 1, cy - 8, 3, 17, kBlack);
  if (player.isPlaying() && !player.isPaused()) {
    epd.fillRect(cx - 11, cy - 13, 8, 26, kWhite);
    epd.fillRect(cx + 3, cy - 13, 8, 26, kWhite);
  } else {
    epd.fillTriangle(cx - 8, cy - 15, cx - 8, cy + 15, cx + 15, cy, kWhite);
  }

  // Progress bar + time.
  const uint32_t pos = player.isPlaying() ? player.positionMs() : 0;
  const uint32_t dur = player.durationMs();
  epd.setFont(&FreeSans9pt7b);
  drawCentered("vol " + String(player.volume()) + "%", 150);
  const int16_t barX = 15, barY = 160, barW = kW - 30, barH = 8;
  epd.drawRect(barX, barY, barW, barH, kBlack);
  if (dur > 0) {
    const int16_t fill = static_cast<int64_t>(barW - 4) * min(pos, dur) / dur;
    epd.fillRect(barX + 2, barY + 2, fill, barH - 4, kBlack);
  }
  drawCentered(formatTime(pos) + " / " + formatTime(dur), 192);
}

// Returns true when the screen needs a redraw.
bool onPlayerTap(uint16_t x, uint16_t y) {
  if (!sdReady || songPath.isEmpty()) return false;
  const bool buttonRow = y > kButtonY - 35 && y < kButtonY + 35;
  if (buttonRow && (x < kVolDownX + 30 || x > kVolUpX - 30)) {
    const int step = x < kW / 2 ? -kVolumeStep : kVolumeStep;
    player.setVolume(constrain(player.volume() + step, 0, 100));
    Serial.printf("Volume: %u%%\n", player.volume());
    return true;
  }
  if (player.isPlaying()) {
    player.togglePause();
  } else {
    player.play(songPath.c_str());
  }
  Serial.printf("Player: %s\n", !player.isPlaying() ? "stopped" : player.isPaused() ? "paused" : "playing");
  return false;  // play state changes are picked up by the loop
}

// ---------- refresh test screen ----------

// Back-to-back partial refreshes of moving content, never a full refresh unless
// tapped. Watch for leftover "shadows" and note the counter when they appear.
void drawRefreshTest(int frame, int partials, uint32_t lastMs) {
  epd.fillScreen(kWhite);
  drawHeader("Refresh test");

  epd.setFont(&FreeSansBold18pt7b);
  drawCentered(String(partials), 70);
  epd.setFont(&FreeSans9pt7b);
  drawCentered("partials since full", 92);

  // Bouncing square across a lane, plus a block that inverts every frame.
  const int16_t lane = kW - 30 - 40;
  int16_t pos = (frame * 17) % (2 * lane);
  if (pos > lane) pos = 2 * lane - pos;
  epd.fillRect(10 + pos, 104, 30, 30, kBlack);
  epd.fillRect(kW - 34, 104, 24, 30, frame % 2 ? kBlack : kWhite);
  epd.drawRect(kW - 34, 104, 24, 30, kBlack);

  char line[32];
  snprintf(line, sizeof(line), "%lu ms  (%.1f fps)", lastMs, lastMs ? 1000.0f / lastMs : 0.0f);
  drawCentered(line, 160);
  drawCentered("tap = full refresh", 188);
}

// ---------- refresh scheduling ----------

// Starts a refresh without waiting for the panel to finish.
// autoFull: clear ghosting with a full refresh every kPartialRefreshesPerFull partials.
void refresh(bool full, bool autoFull = true) {
  if (full || (autoFull && partialsSinceFull >= kPartialRefreshesPerFull)) {
    epd.refreshFull();
    partialsSinceFull = 0;
  } else {
    epd.refreshPartial();
    partialsSinceFull++;
  }
}

void showScreen() {
  if (screen == Screen::Player) {
    drawPlayer();
  } else {
    drawRefreshTest(0, 0, 0);
  }
  refresh(true);
}

// On battery the board only stays on while PIN_VBAT_PWR is held high.
// Holding PWR for 2 s releases it. The button must first be let go after
// the long press that powered the board on.
void checkPowerButton() {
  static bool armed = false;
  static uint32_t pressedAt = 0;

  const bool pressed = digitalRead(PIN_BTN_PWR) == LOW;
  if (!pressed) {
    armed = true;
    pressedAt = 0;
    return;
  }
  if (!armed) return;
  if (pressedAt == 0) pressedAt = millis();
  if (millis() - pressedAt < kPowerOffHoldMs) return;

  Serial.println("Power off");
  player.stop();
  epd.fillScreen(kWhite);
  epd.setFont(&FreeSansBold9pt7b);
  epd.setTextColor(kBlack);
  drawCentered("sleeping...", 105);
  epd.refreshFull();
  epd.powerOff();
  digitalWrite(PIN_VBAT_PWR, LOW);
  delay(1000);  // still running here when powered over USB
  armed = false;
}

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
    Serial.println("SD: mount failed");
    return;
  }
  Serial.printf("SD: %llu MB card, %llu MB used\n", SD_MMC.cardSize() >> 20, SD_MMC.usedBytes() >> 20);
  songPath = findFirstMp3("/music");
  songTitle = titleFromPath(songPath);
  Serial.printf("SD: song '%s'\n", songPath.c_str());
}

}  // namespace

void setup() {
  // Latch power first so the board survives releasing PWR on battery.
  pinMode(PIN_VBAT_PWR, OUTPUT);
  digitalWrite(PIN_VBAT_PWR, HIGH);
  pinMode(PIN_BTN_PWR, INPUT_PULLUP);
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
  attachInterrupt(PIN_BTN_BOOT, onBootButton, FALLING);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial.begin(115200);
  delay(1500);  // give the USB serial monitor time to attach
  Serial.printf("\n=== Dotty ===\nPSRAM %lu KB, heap %lu KB\n", ESP.getPsramSize() / 1024,
                ESP.getFreeHeap() / 1024);

  // The touch controller is powered from the audio rail, so switch it on before touch.begin().
  pinMode(PIN_AUDIO_PWR, OUTPUT);
  digitalWrite(PIN_AUDIO_PWR, LOW);
  epd.begin();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  initStorage();
  Serial.printf("Touch: %s\n", touch.begin() ? "ok" : "FAILED");
  Serial.printf("Audio: %s\n", player.begin() ? "ok" : "FAILED");
  player.setVolume(kVolumePercent);

  showScreen();
  digitalWrite(PIN_LED, HIGH);
}

void loop() {
  static uint32_t lastPlayerDraw = 0;
  static uint32_t lastPositionS = UINT32_MAX;
  static bool lastPlaying = false, lastPaused = false;
  static int testFrame = 0;

  checkPowerButton();

  if (bootPressed) {
    bootPressed = false;
    screen = screen == Screen::Player ? Screen::RefreshTest : Screen::Player;
    Serial.printf("Screen: %s\n", screen == Screen::Player ? "player" : "refresh test");
    showScreen();
    return;
  }

  const Touch::Gesture gesture = touch.poll();
  const bool tapped = gesture == Touch::Gesture::Tap;
  const bool longPress = gesture == Touch::Gesture::LongPress;
  if (gesture != Touch::Gesture::None) {
    Serial.printf("%s at (%u, %u)\n", tapped ? "Tap" : "Long press", touch.x(), touch.y());
  }

  if (screen == Screen::RefreshTest) {
    const bool full = tapped || longPress;
    drawRefreshTest(++testFrame, full ? 0 : partialsSinceFull + 1, lastRefreshMs);
    const uint32_t start = millis();
    refresh(full, false);
    epd.waitBusy();  // the test measures the panel's real update time
    lastRefreshMs = millis() - start;
    Serial.printf("Refresh test: %s #%d, %lu ms\n", partialsSinceFull ? "partial" : "FULL",
                  partialsSinceFull, lastRefreshMs);
    return;
  }

  static bool redraw = false;
  static bool fullRequested = false;
  if (tapped) redraw |= onPlayerTap(touch.x(), touch.y());
  if (longPress) fullRequested = true;  // long press: full refresh to clear ghosting

  // Input is handled above immediately; drawing waits until the panel is free.
  if (epd.isBusy()) {
    delay(5);
    return;
  }
  // Redraw on any state change, and once a second while playing.
  const bool playing = player.isPlaying(), paused = player.isPaused();
  const uint32_t positionS = player.positionMs() / 1000;
  const bool stateChanged = playing != lastPlaying || paused != lastPaused;
  const bool tick = playing && !paused && positionS != lastPositionS &&
                    millis() - lastPlayerDraw >= kPlayerRefreshMs;
  if (fullRequested || redraw || stateChanged || tick) {
    lastPlaying = playing;
    lastPaused = paused;
    lastPositionS = positionS;
    lastPlayerDraw = millis();
    drawPlayer();
    refresh(fullRequested);
    fullRequested = false;
    redraw = false;
  }
  delay(10);
}
