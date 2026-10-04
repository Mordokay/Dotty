#include <Arduino.h>
#include <FS.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <SD_MMC.h>
#include <Wire.h>

#include "audio_player.h"
#include "battery.h"
#include "board_pins.h"
#include "epd_display.h"
#include "images/sleep_portrait.h"
#include "lock_screen.h"
#include "log.h"
#include "power.h"
#include "rtc_clock.h"
#include "touch.h"
#include "ui.h"

namespace {

constexpr uint32_t kPowerOffHoldMs = 2000;
constexpr uint32_t kAutoLockMs = 2 * 60 * 1000;
constexpr uint32_t kPlayerRefreshMs = 1000;
constexpr uint32_t kBatteryLogIntervalMs = 10 * 60 * 1000;
constexpr int kPartialRefreshesPerFull = 50;
constexpr uint8_t kVolumePercent = 80;
constexpr uint8_t kVolumeStep = 10;
constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

constexpr int16_t kButtonY = 100;
constexpr int16_t kVolDownX = 32;
constexpr int16_t kVolUpX = kW - 32;

enum class Screen { Player, RefreshTest, Lock };

EpdDisplay epd;
AudioPlayer player;
Touch touch;
RtcClock rtc;

Screen screen = Screen::Player;
Screen screenBeforeLock = Screen::Player;
bool sdReady = false;
String songPath;
String songTitle;
int partialsSinceFull = 0;
uint32_t lastRefreshMs = 0;
uint32_t lastInteraction = 0;
volatile bool bootPressed = false;
bool pwrArmed = false;            // PWR presses count only after it has been released once
bool peripheralsAsleep = false;   // touch hibernating, codec + amp powered down

void IRAM_ATTR onBootButton() {
  bootPressed = true;
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

// ---------- lock screen ----------

// What the lock screen currently shows, to know when it needs a redraw.
int lockShownMinute = -1;
bool lockShownPlaying = false;

void drawLock() {
  LockScreenInfo info = {};
  info.timeValid = rtc.read(info.time);
  info.batteryPercent = batteryPercent(batteryMillivolts());
  if (musicPlaying()) info.nowPlaying = songTitle;
  drawLockScreen(epd, info);
  lockShownMinute = info.time.tm_min;
  lockShownPlaying = musicPlaying();
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

// Draws the current screen from scratch with a full refresh (clean transition).
void showScreen() {
  switch (screen) {
    case Screen::Player:
      drawPlayer();
      break;
    case Screen::RefreshTest:
      drawRefreshTest(0, 0, 0);
      break;
    case Screen::Lock:
      drawLock();
      break;
  }
  refresh(true);
}

void lock() {
  if (screen == Screen::Lock) return;
  screenBeforeLock = screen;
  screen = Screen::Lock;
  LOGI("ui", "locked%s", musicPlaying() ? " (music keeps playing)" : "");
  showScreen();
}

// Touch and the codec are only needed while unlocked or playing.
void sleepPeripherals() {
  if (peripheralsAsleep) return;
  touch.sleep();
  player.powerDown();
  peripheralsAsleep = true;
}

void wakePeripherals() {
  if (!peripheralsAsleep) return;
  power::setAudioRail(true);
  delay(5);
  touch.wake();
  player.powerUp();
  peripheralsAsleep = false;
}

void unlock() {
  if (screen != Screen::Lock) return;
  wakePeripherals();
  screen = screenBeforeLock;
  lastInteraction = millis();
  LOGI("ui", "unlocked");
  showScreen();
}

// ---------- power button ----------

[[noreturn]] void powerOff() {
  LOGI("power", "power off");
  player.stop();
  epd.fillScreen(kWhite);
  epd.drawBitmap((kW - kSleepPortraitWidth) / 2, 12, kSleepPortrait, kSleepPortraitWidth,
                 kSleepPortraitHeight, kBlack);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, "press PWR to wake", 190);
  epd.refreshFull();
  epd.powerOff();  // waits for the refresh; the picture stays without power
  power::shutdown();
}

// Short press (released before 2 s) toggles the lock; holding 2 s powers off.
// On battery the board only stays on while PIN_VBAT_PWR is held high, and the
// button is still down after the press that powered it on, so it is ignored
// until released once.
void checkPowerButton() {
  static uint32_t pressedAt = 0;
  static bool longHandled = false;

  const bool pressed = digitalRead(PIN_BTN_PWR) == LOW;
  if (!pressed) {
    if (pwrArmed && pressedAt != 0 && !longHandled) {
      screen == Screen::Lock ? unlock() : lock();
    }
    pwrArmed = true;
    pressedAt = 0;
    longHandled = false;
    return;
  }
  if (!pwrArmed || longHandled) return;
  if (pressedAt == 0) pressedAt = millis();
  if (millis() - pressedAt >= kPowerOffHoldMs) {
    longHandled = true;
    powerOff();
  }
}

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

// ---------- per-screen loop handlers ----------

void loopRefreshTest(bool fullRequested) {
  static int frame = 0;
  drawRefreshTest(++frame, fullRequested ? 0 : partialsSinceFull + 1, lastRefreshMs);
  const uint32_t start = millis();
  refresh(fullRequested, false);
  epd.waitBusy();  // the test measures the panel's real update time
  lastRefreshMs = millis() - start;
  LOGI("refresh", "%s #%d, %lu ms", partialsSinceFull ? "partial" : "FULL", partialsSinceFull,
       lastRefreshMs);
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
    refresh(fullRequested);
  }
}

// Redraws once a minute, or when music starts/stops. With no wake lock held
// (and no computer attached) the CPU light-sleeps between the minute updates.
void loopLock() {
  static uint32_t lastCheck = 0;
  const bool canSleep = power::wakeLocks() == 0 && !power::usbHostConnected();

  if (!canSleep) {
    if (millis() - lastCheck < 1000 || epd.isBusy()) return;
    lastCheck = millis();
    tm now;
    rtc.read(now);
    if (now.tm_min != lockShownMinute || musicPlaying() != lockShownPlaying) {
      drawLock();
      refresh(false);
    }
    return;
  }

  epd.waitBusy();
  sleepPeripherals();
  tm now;
  rtc.read(now);
  if (now.tm_min != lockShownMinute || lockShownPlaying) {
    drawLock();
    refresh(false);
    epd.waitBusy();
    rtc.read(now);
  }

  // Wake just after the next minute starts. The audio rail goes off while asleep
  // and comes back on at wake so the RTC is reachable over I2C again.
  const uint32_t sleepMs = (60 - now.tm_sec) * 1000 + 200;
  power::setAudioRail(false);
  const bool byButton = power::lightSleep(sleepMs);
  power::setAudioRail(true);
  delay(2);
  if (byButton) {
    LOGI("power", "woken by PWR");
    pwrArmed = false;  // this press unlocks; its release must not lock again
    unlock();
  }
}

}  // namespace

void setup() {
  // Latch power first so the board survives releasing PWR on battery.
  power::begin();
  pinMode(PIN_BTN_PWR, INPUT_PULLUP);
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
  attachInterrupt(PIN_BTN_BOOT, onBootButton, FALLING);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);  // green LED on while booting (active low)

  Serial.begin(115200);
  dlog::begin();
  LOGI("boot", "=== Dotty === PSRAM %lu KB, heap %lu KB", ESP.getPsramSize() / 1024,
       ESP.getFreeHeap() / 1024);

  // The unpowered codec would hold the shared I2C bus down: switch the audio rail on first.
  power::setAudioRail(true);
  epd.begin();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  initStorage();
  LOGI("boot", "touch %s", touch.begin() ? "ok" : "FAILED");
  LOGI("boot", "audio %s", player.begin() ? "ok" : "FAILED");
  LOGI("boot", "rtc %s", rtc.begin(Wire) ? "ok" : "FAILED");
  player.setVolume(kVolumePercent);

  lastInteraction = millis();
  showScreen();
  digitalWrite(PIN_LED, HIGH);
}

void loop() {
  static uint32_t lastBatteryLog = 0;
  if (millis() - lastBatteryLog >= kBatteryLogIntervalMs || lastBatteryLog == 0) {
    lastBatteryLog = millis();
    const uint32_t mv = batteryMillivolts();
    LOGI("power", "battery %lu mV (%u%%)%s", mv, batteryPercent(mv),
         power::usbHostConnected() ? ", USB host" : "");
  }

  power::setWakeLock(power::kWakeLockAudio, musicPlaying());
  checkPowerButton();

  const Touch::Gesture gesture = touch.poll();
  const bool tapped = gesture == Touch::Gesture::Tap;
  const bool longPress = gesture == Touch::Gesture::LongPress;
  const bool boot = bootPressed;
  bootPressed = false;

  if (screen == Screen::Lock) {
    // Locked: touch and BOOT are ignored, only PWR unlocks.
    loopLock();
    delay(10);
    return;
  }

  if (gesture != Touch::Gesture::None || boot) lastInteraction = millis();
  if (gesture != Touch::Gesture::None) {
    LOGI("touch", "%s at (%u, %u)", tapped ? "tap" : "long press", touch.x(), touch.y());
  }
  if (millis() - lastInteraction >= kAutoLockMs) {
    LOGI("ui", "idle for %lu s", kAutoLockMs / 1000);
    lock();
    return;
  }

  if (boot) {
    screen = screen == Screen::Player ? Screen::RefreshTest : Screen::Player;
    LOGI("ui", "screen: %s", screen == Screen::Player ? "player" : "refresh test");
    showScreen();
    return;
  }

  if (screen == Screen::RefreshTest) {
    loopRefreshTest(tapped || longPress);
    return;
  }

  static bool redraw = false;
  static bool fullRequested = false;
  if (tapped) redraw |= onPlayerTap(touch.x(), touch.y());
  if (longPress) fullRequested = true;  // long press: full refresh to clear ghosting

  // Input is handled above immediately; drawing waits until the panel is free.
  if (!epd.isBusy()) {
    loopPlayer(redraw, fullRequested);
    redraw = false;
    fullRequested = false;
  }
  delay(10);
}
