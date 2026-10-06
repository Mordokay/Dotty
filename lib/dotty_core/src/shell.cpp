#include "shell.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Preferences.h>
#include <Wire.h>

#include "battery.h"

#include <esp_core_dump.h>
#include <esp_task_wdt.h>
#include "board_pins.h"
#include "cartridge.h"
#include "core_ble.h"
#include "lock_screen.h"
#include "log.h"
#include "net.h"
#include "storage.h"
#include "power.h"
#include "ui.h"

namespace shell {

EpdDisplay epd;
Touch touch;
RtcClock rtc;

namespace {

// The phone's UTC offset (core.time {utcOffset}, NVS clock/utcOffset), read on first use.
constexpr int32_t kOffsetUnknown = INT32_MIN;     // not read from NVS yet
constexpr int32_t kOffsetUnread = INT32_MIN + 1;  // no phone has sent one
int32_t cachedOffset = kOffsetUnknown;

constexpr uint32_t kPowerOffHoldMs = 2000;
constexpr uint32_t kLauncherComboMs = 1000;
constexpr uint32_t kConfirmAfterMs = 5000;  // a new firmware's trial (bootloader rollback)
constexpr uint32_t kAutoLockMs = 2 * 60 * 1000;
constexpr uint32_t kBatteryLogIntervalMs = 10 * 60 * 1000;
constexpr uint32_t kPowerCardMs = 2500;
constexpr uint32_t kPwrDebounceMs = 30;
constexpr uint32_t kPwrLockoutMs = 600;  // after a lock or unlock, PWR is ignored this long
uint32_t lastLockToggle = 0;
// A cartridge's loop that doesn't come back for this long is stuck: the task watchdog
// restarts Dotty (saving a core dump to flash). Well above the longest legit wait in a loop
// pass (joining Wi-Fi, ~20 s; a locked minute of light sleep). Not in the launcher, whose
// Bluetooth installs can block longer.
constexpr uint32_t kLoopWatchdogMs = 90000;
bool loopWatchdog = false;
constexpr int kPartialRefreshesPerFull = 50;
constexpr int16_t kW = EpdDisplay::kSize;

Config cfg;
bool isLocked = false;
int partials = 0;
uint32_t lastInteraction = 0;
bool peripheralsAsleep = false;  // touch hibernating, firmware peripherals powered down

// Buttons. PWR presses count only after it has been released once (it is still held
// right after a battery power-on). BOOT is latched by an interrupt so a click is not
// missed while the loop is busy, and reported on release.
bool pwrArmed = false;
uint32_t pwrPressedAt = 0;
bool pwrLongHandled = false;
volatile bool bootWentDown = false;
uint32_t comboStartedAt = 0;
bool comboUsed = false;  // BOOT + PWR were held together: neither counts as a click

// Next time onLockedWake is due (millis keeps counting through light sleep).
uint32_t nextLockedWake = 0;

// Runs the firmware's periodic locked work when due. True if the lock screen changed.
bool runLockedWake() {
  if (!cfg.onLockedWake || cfg.lockedWakeSeconds == 0) return false;
  if (static_cast<int32_t>(millis() - nextLockedWake) < 0) return false;
  nextLockedWake = millis() + cfg.lockedWakeSeconds * 1000;
  return cfg.onLockedWake();
}

// The power card (plugged in / unplugged while unlocked): when it goes away, and the power
// source last announced.
uint32_t powerCardUntil = 0;
bool announcedExternal = false;

// What the lock screen currently shows, to know when it needs a redraw.
int lockShownMinute = -1;
String lockShownText;
int lockShownBattery = -1;

// Percentage and charging/low state in one number, to notice any change.
int batteryKey() {
  return battery::percent() * 4 + (battery::external() ? 2 : 0) + (battery::low() ? 1 : 0);
}

void IRAM_ATTR onBootButton() {
  bootWentDown = true;
}

String nowPlaying() {
  return cfg.nowPlaying ? cfg.nowPlaying() : String();
}

// Draws the lock screen; true if it needs a full refresh (the firmware's own, see
// Config::lockScreen).
bool drawLock() {
  LockScreenInfo info = {};
  info.timeValid = rtc.read(info.time);
  info.batteryPercent = battery::percent();
  info.externalPower = battery::external();
  info.charging = battery::charging();
  info.batteryLow = battery::low();
  info.nowPlaying = nowPlaying();
  info.widget = cfg.drawLockWidget;
  bool full = false;
  if (cfg.lockScreen) {
    full = cfg.lockScreen(epd, info);
  } else {
    drawLockScreen(epd, info);
  }
  lockShownMinute = info.time.tm_min;
  lockShownText = info.nowPlaying;
  lockShownBattery = batteryKey();
  return full;
}

void sleepPeripherals() {
  if (peripheralsAsleep) return;
  touch.sleep();
  if (cfg.sleepApp) cfg.sleepApp();
  peripheralsAsleep = true;
}

void wakePeripherals() {
  if (!peripheralsAsleep) return;
  power::setAudioRail(true);
  delay(5);
  touch.wake();
  if (cfg.wakeApp) cfg.wakeApp();
  peripheralsAsleep = false;
}

void unlock() {
  if (!isLocked) return;
  lastLockToggle = millis();
  wakePeripherals();
  ble::start();
  isLocked = false;
  lastInteraction = millis();
  LOGI("ui", "unlocked");
  showApp();
}

[[noreturn]] void powerOff() {
  LOGI("power", "power off");
  cartridge::confirmHealthy();  // switched off on purpose: not a crash to roll back
  if (cfg.beforePowerOff) cfg.beforePowerOff();
  epd.fillScreen(EpdDisplay::kWhite);
  if (cfg.offPictureCount > 0) {
    const Picture &pic = cfg.offPictures[esp_random() % cfg.offPictureCount];
    // Centred in the area above the "press PWR" line.
    epd.drawBitmap((kW - pic.width) / 2, (170 - pic.height) / 2, pic.bitmap, pic.width,
                   pic.height, EpdDisplay::kBlack);
  }
  epd.setFont(&FreeSans9pt7b);
  epd.setTextColor(EpdDisplay::kBlack);
  ui::drawCentered(epd, "press PWR to wake", 190);
  epd.refreshFull();
  epd.powerOff();  // waits for the refresh; the picture stays without power
  power::shutdown();
}

// Short PWR press toggles the lock, a 2 s hold powers off, BOOT + PWR for 1 s goes
// back to the launcher. Returns true on a BOOT click.
bool handleButtons() {
  // PWR is debounced: its contacts chatter on release, and one bounce read as a second press
  // toggled a fresh lock straight back to unlocked. A change counts once it holds 30 ms, and
  // presses right after a lock/unlock are ignored.
  static bool pwrStable = false;
  static bool pwrRaw = false;
  static uint32_t pwrRawSince = 0;
  const bool rawPwr = digitalRead(PIN_BTN_PWR) == LOW;
  if (rawPwr != pwrRaw) {
    pwrRaw = rawPwr;
    pwrRawSince = millis();
  }
  if (pwrRaw != pwrStable && millis() - pwrRawSince >= kPwrDebounceMs) pwrStable = pwrRaw;
  const bool pwr = pwrStable;
  const bool pwrPaused = millis() - lastLockToggle < kPwrLockoutMs;  // PWR handling waits
  const bool boot = digitalRead(PIN_BTN_BOOT) == LOW;

  if (pwr && boot) {
    if (comboStartedAt == 0) comboStartedAt = millis();
    comboUsed = true;
    if (millis() - comboStartedAt >= kLauncherComboMs && !cartridge::isLauncher()) {
      cartridge::rebootToLauncher();
    }
  } else {
    comboStartedAt = 0;
  }

  bool bootClick = false;
  if (bootWentDown && !boot) {
    bootWentDown = false;
    bootClick = !comboUsed;
  }

  if (pwrPaused) {
    // Right after a lock/unlock: neither a press nor a release counts yet.
  } else if (!pwr) {
    if (pwrArmed && pwrPressedAt != 0 && !pwrLongHandled && !comboUsed) {
      LOGI("ui", "PWR pressed");
      isLocked ? unlock() : lock();
    }
    pwrArmed = true;
    pwrPressedAt = 0;
    pwrLongHandled = false;
  } else if (pwrArmed && !pwrLongHandled && !comboUsed) {
    if (pwrPressedAt == 0) pwrPressedAt = millis();
    if (millis() - pwrPressedAt >= kPowerOffHoldMs) {
      pwrLongHandled = true;
      powerOff();
    }
  }

  if (!pwr && !boot) comboUsed = false;
  return bootClick;
}

// Redraws once a minute, or when the bottom line changes. With no wake lock held (and
// no computer attached) the CPU light-sleeps between the minute updates.
void loopLock() {
  static uint32_t lastCheck = 0;
  if (dlog::takeKey() == 'k') {  // developer aid: serial k unlocks (and locks, in update())
    unlock();
    return;
  }
  const bool canSleep = power::wakeLocks() == 0 && !power::usbHostConnected();

  if (!canSleep) {
    if (millis() - lastCheck < 1000 || epd.isBusy()) return;
    lastCheck = millis();
    tm now;
    rtc.read(now);
    const bool appChanged = runLockedWake();
    if (appChanged || now.tm_min != lockShownMinute || nowPlaying() != lockShownText ||
        batteryKey() != lockShownBattery) {
      refresh(drawLock());
    }
    return;
  }

  epd.waitBusy();
  sleepPeripherals();
  ble::stop();  // the BLE controller can't light-sleep on this board (no 32 kHz crystal)
  tm now;
  rtc.read(now);
  const bool appChanged = runLockedWake();
  if (appChanged || now.tm_min != lockShownMinute || nowPlaying() != lockShownText ||
        batteryKey() != lockShownBattery) {
    refresh(drawLock());
    epd.waitBusy();
    rtc.read(now);
  }

  // Wake just after the next minute starts, or earlier when the firmware's own locked
  // work is due. The audio rail goes off while asleep and comes back on at wake so the
  // RTC is reachable over I2C again.
  uint32_t sleepMs = (60 - now.tm_sec) * 1000 + 200;
  if (cfg.onLockedWake && cfg.lockedWakeSeconds) {
    const int32_t untilApp = static_cast<int32_t>(nextLockedWake - millis());
    sleepMs = min<uint32_t>(sleepMs, max<int32_t>(untilApp, 100));
  }
  power::setAudioRail(false);
  if (loopWatchdog) esp_task_wdt_reset();  // a minute asleep is not a stuck loop
  const bool byButton = power::lightSleep(sleepMs);
  if (loopWatchdog) esp_task_wdt_reset();
  power::setAudioRail(true);
  delay(2);
  // Only a real press unlocks: the button must still be down as we wake (a press lasts far
  // longer than waking up). Anything else is logged and Dotty goes back to sleep next pass.
  const bool held = digitalRead(PIN_BTN_PWR) == LOW;
  if (byButton && held) {
    LOGI("power", "woken by PWR");
    pwrArmed = false;  // this press unlocks; its release must not lock again
    unlock();
  } else if (byButton) {
    LOGW("power", "woke for PWR, but it isn't pressed: staying locked");
  }
}

// While an iPhone pairs, Dotty shows the code to type on the phone.
// The black title bar of the pairing screens (the same height as the nav bar).
void drawTitleBar(const char *title) {
  epd.fillScreen(EpdDisplay::kWhite);
  epd.fillRect(0, 0, kW, 45, EpdDisplay::kBlack);
  epd.setTextColor(EpdDisplay::kWhite);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, title, 29);
  epd.setTextColor(EpdDisplay::kBlack);
}

void drawPairing(uint32_t code) {
  char digits[8];
  snprintf(digits, sizeof(digits), "%03lu %03lu", code / 1000, code % 1000);
  drawTitleBar("Pairing");
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, "Type this code", 78);
  ui::drawCentered(epd, "on your iPhone", 98);
  epd.setFont(&FreeSansBold24pt7b);
  ui::drawCentered(epd, digits, 152);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, ble::name(), 188);
}

void drawPairingResult(bool success) {
  drawTitleBar("Pairing");
  if (success) {
    // A tick in a circle.
    epd.fillCircle(kW / 2, 92, 26, EpdDisplay::kBlack);
    for (int d = -2; d <= 2; d++) {
      epd.drawLine(kW / 2 - 12, 92 + d, kW / 2 - 3, 101 + d, EpdDisplay::kWhite);
      epd.drawLine(kW / 2 - 3, 101 + d, kW / 2 + 13, 84 + d, EpdDisplay::kWhite);
    }
  }
  epd.setFont(&FreeSansBold12pt7b);
  ui::drawCentered(epd, success ? "Paired!" : "That didn't work", success ? 150 : 100);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, success ? "Nice to meet you" : "Try again from the app", success ? 176 : 130);
}

void handlePairing() {
  static bool showing = false;
  uint32_t code;
  if (ble::pairingCode(code) && !showing) {
    showing = true;
    lastInteraction = millis();
    if (isLocked) {
      wakePeripherals();
      isLocked = false;
    }
    epd.waitBusy();
    drawPairing(code);
    refresh(true);
  }
  bool success;
  if (ble::takePairingResult(success)) {
    showing = false;
    epd.waitBusy();
    drawPairingResult(success);
    refresh(false);
    epd.waitBusy();
    delay(1500);
    showApp();
  }
}

// After a crash or watchdog restart: say so in the log, with the summary of the core dump
// the crash left in flash (the full dump stays there for espcoredump until the next crash).
void reportPreviousCrash() {
  const esp_reset_reason_t why = esp_reset_reason();
  if (why != ESP_RST_PANIC && why != ESP_RST_TASK_WDT && why != ESP_RST_INT_WDT && why != ESP_RST_WDT) return;
  LOGW("boot", "the last run crashed (reset reason %d)", static_cast<int>(why));
  auto *summary = static_cast<esp_core_dump_summary_t *>(malloc(sizeof(esp_core_dump_summary_t)));
  if (summary && esp_core_dump_image_check() == ESP_OK && esp_core_dump_get_summary(summary) == ESP_OK) {
    char bt[16 * 11 + 1] = "";
    for (uint32_t i = 0; i < summary->exc_bt_info.depth && i < 16; i++) {
      snprintf(bt + strlen(bt), sizeof(bt) - strlen(bt), " 0x%08lx", static_cast<unsigned long>(summary->exc_bt_info.bt[i]));
    }
    LOGW("crash", "task %s, pc 0x%08lx, backtrace%s", summary->exc_task,
         static_cast<unsigned long>(summary->exc_pc), bt);
  }
  free(summary);
}

void logBattery() {
  static uint32_t lastLog = 0;
  if (lastLog != 0 && millis() - lastLog < kBatteryLogIntervalMs) return;
  lastLog = millis();
  LOGI("power", "battery %lu mV now, %lu mV smoothed, %u%%%s%s", battery::readMillivolts(),
       battery::millivolts(), battery::percent(), battery::external() ? ", external power" : "",
       power::usbHostConnected() ? " (USB host)" : "");
}

// The battery is flat: say so on the screen (the e-paper keeps it) and switch off. Dotty
// stays off until it's charged; on battery the next PWR press shows this again.
[[noreturn]] void batteryEmptyOff() {
  LOGW("power", "battery empty (%lu mV): switching off", battery::millivolts());
  cartridge::confirmHealthy();
  if (cfg.beforePowerOff) cfg.beforePowerOff();
  epd.waitBusy();
  drawBatteryEmpty(epd);
  epd.refreshFull();
  epd.powerOff();
  power::shutdown();
}

}  // namespace

void begin(const Config &config) {
  cfg = config;

  // Latch power first so the board survives releasing PWR on battery.
  power::begin();
  pinMode(PIN_BTN_PWR, INPUT_PULLUP);
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
  attachInterrupt(PIN_BTN_BOOT, onBootButton, FALLING);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);  // green LED on while booting (active low)

  Serial.begin(115200);
  dlog::begin();
  const CartridgeInfo &me = cartridge::self();
  LOGI("boot", "=== Dotty %s %s === PSRAM %lu KB, heap %lu KB", me.name, me.version,
       ESP.getPsramSize() / 1024, ESP.getFreeHeap() / 1024);
  reportPreviousCrash();
  // A launcher update that failed its trial left the launcher slot unusable: Rescue puts the
  // last good launcher back (it needs the launcher to switch cartridges and update).
  if (!cartridge::isLauncher() && cartridge::launcherBroken()) {
    LOGW("boot", "the launcher failed its trial: handing over to Rescue");
    cartridge::rebootToRescue();
  }
  if (!cartridge::isLauncher()) {
    esp_task_wdt_config_t wdt = {};
    wdt.timeout_ms = kLoopWatchdogMs;
    wdt.idle_core_mask = 1 << 0;  // as the core sets it up: CPU0's idle task
    wdt.trigger_panic = true;
    esp_task_wdt_reconfigure(&wdt);
    enableLoopWDT();  // Arduino feeds it after every loop()
    loopWatchdog = true;
  }

  // The unpowered codec would hold the shared I2C bus down: switch the audio rail on first.
  power::setAudioRail(true);
  epd.begin();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  LOGI("boot", "touch %s", touch.begin() ? "ok" : "FAILED");
  LOGI("boot", "rtc %s", rtc.begin(Wire) ? "ok" : "FAILED");
  battery::begin();
  announcedExternal = battery::external();  // no card for the state Dotty starts in
  // Switched on (PWR) with a flat battery: show why and go back off.
  if (!battery::external() && battery::readMillivolts() < battery::kEmptyMv) batteryEmptyOff();
  ble::begin();
  net::registerCommands();
  storage::registerCommands();
  // The phone sends its local time on every connection: the RTC drifts and has no other
  // source in most firmwares. {local}: seconds since 1970 in local time (no zone).
  ble::on("core.time", [](JsonObjectConst args, JsonObject reply) {
    const time_t local = args["local"] | (time_t)0;
    if (local < 1700000000) {
      reply["ok"] = false;
      reply["error"] = "local time needed";
      return;
    }
    tm now;
    time_t current = 0;
    if (rtc.read(now)) {
      now.tm_isdst = 0;
      current = mktime(&now);  // TZ is unset (UTC), so this undoes gmtime_r exactly
    }
    reply["drift"] = (long)(current - local);
    if (labs((long)(current - local)) >= 2) setLocalTime(local);
    if (args["utcOffset"].is<int32_t>()) {
      const int32_t offset = args["utcOffset"];
      int32_t known = 0;
      if (!utcOffset(known) || known != offset) {
        Preferences p;
        p.begin("clock", false);
        p.putInt("utcOffset", offset);
        p.end();
        cachedOffset = offset;
      }
    }
  });
  lastInteraction = millis();
}

// 's' on the serial monitor: the screen as hex, for tools/screenshot.py (only with a
// computer attached; the serial port stays quiet otherwise).
void sendScreenshot() {
  if (!HWCDC::isPlugged()) return;
  const uint8_t *buf = epd.getBuffer();
  const size_t bytes = EpdDisplay::kSize * EpdDisplay::kSize / 8;
  Serial.printf("\n#SCREEN %d %d\n", EpdDisplay::kSize, EpdDisplay::kSize);
  char line[2 * 50 + 2];
  for (size_t i = 0; i < bytes; i += 50) {
    size_t n = 0;
    for (size_t j = i; j < i + 50 && j < bytes; j++) n += snprintf(line + n, sizeof(line) - n, "%02x", buf[j]);
    line[n++] = '\n';
    Serial.write(reinterpret_cast<uint8_t *>(line), n);
    Serial.flush();
  }
  Serial.print("#END\n");
}

bool update(Input &input) {
  // Running fine for a few seconds: confirm this firmware so the bootloader keeps it.
  static bool confirmed = false;
  if (!confirmed && millis() > kConfirmAfterMs) {
    confirmed = true;
    cartridge::confirmHealthy();
  }
  const char key = dlog::takeKey();
  if (key == 's') sendScreenshot();
  input.key = key == 's' ? 0 : key;
  if (key == 'k' && !isLocked) {
    lock();
    return false;
  }
  if (key == 'p' && !isLocked) {  // developer aid: the pairing screens (code, then the result)
    static int step = 0;
    if (step % 3 == 0) drawPairing(123456);
    else drawPairingResult(step % 3 == 1);
    step++;
    refresh(false);
    input.key = 0;
  }
  battery::poll();
  if (battery::empty()) batteryEmptyOff();
  logBattery();
  if (ble::poll() > 0 && !isLocked) lastInteraction = millis();  // the app in use: no auto-lock
  handlePairing();
  power::setWakeLock(power::kWakeLockBle, ble::connected());
  const bool bootClick = handleButtons();
  Touch::Gesture gesture = touch.poll();
  // Developer aid: serial keys 1-9 tap a 3x3 grid like a phone keypad (1 = top left, the
  // nav bar's left corner; 3 = its right corner), to drive screens from the computer.
  if (key >= '1' && key <= '9' && !isLocked) {
    const int cell = key - '1';
    touch.simulate(cell % 3 * 66 + 22, cell / 3 == 0 ? 20 : cell / 3 * 66 + 33);
    gesture = Touch::Gesture::Tap;
    input.key = 0;
  }
  if ((key == '[' || key == ']') && !isLocked) {  // ...and [ ] swipe right / left
    gesture = key == ']' ? Touch::Gesture::SwipeLeft : Touch::Gesture::SwipeRight;
    input.key = 0;
  }
  if ((key == '{' || key == '}') && !isLocked) {  // { } swipe down / up (scroll back / on)
    gesture = key == '}' ? Touch::Gesture::SwipeUp : Touch::Gesture::SwipeDown;
    input.key = 0;
  }

  if (isLocked) {
    // Locked: touch and BOOT are ignored, only PWR unlocks. The lock screen shows power
    // changes itself, so no card for them afterwards.
    announcedExternal = battery::external();
    loopLock();
    delay(10);
    return false;
  }

  if (gesture != Touch::Gesture::None || bootClick) lastInteraction = millis();
  if (gesture != Touch::Gesture::None) {
    static const char *kNames[] = {"none", "tap", "long press", "swipe up", "swipe down", "swipe left", "swipe right"};
    LOGI("touch", "%s at (%u, %u)", kNames[static_cast<int>(gesture)], touch.x(), touch.y());
  }
  if (millis() - lastInteraction >= kAutoLockMs) {
    LOGI("ui", "idle for %lu s", kAutoLockMs / 1000);
    powerCardUntil = 0;
    lock();
    return false;
  }

  // Plugged in or unplugged: there's no battery icon on the app screens, so say it.
  if (battery::external() != announcedExternal) {
    announcedExternal = battery::external();
    epd.waitBusy();
    drawPowerCard(epd, battery::percent(), battery::external(), battery::charging());
    refresh(false);
    powerCardUntil = millis() + kPowerCardMs;
  }
  if (powerCardUntil) {
    // The card goes away by itself, or with a tap; the app waits meanwhile.
    if (static_cast<int32_t>(millis() - powerCardUntil) < 0 && gesture != Touch::Gesture::Tap) {
      delay(10);
      return false;
    }
    powerCardUntil = 0;
    epd.waitBusy();
    showApp();
    delay(10);
    return false;
  }

  digitalWrite(PIN_LED, HIGH);
  input.gesture = gesture;
  input.boot = bootClick;
  return true;
}

void showApp() {
  cfg.drawApp();
  refresh(true);
}

void refresh(bool full, bool autoFull) {
  if (full || (autoFull && partials >= kPartialRefreshesPerFull)) {
    epd.refreshFull();
    partials = 0;
  } else {
    epd.refreshPartial();
    partials++;
  }
}

int partialsSinceFull() {
  return partials;
}

bool utcOffset(int32_t &seconds) {
  if (cachedOffset == kOffsetUnknown) {
    Preferences p;
    p.begin("clock", true);
    cachedOffset = p.getInt("utcOffset", kOffsetUnread);
    p.end();
  }
  if (cachedOffset == kOffsetUnread) return false;
  seconds = cachedOffset;
  return true;
}

void setLocalTime(time_t local) {
  tm t;
  gmtime_r(&local, &t);  // the epoch already holds local time: no zone to apply
  rtc.write(t);
  LOGI("clock", "set to %04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
       t.tm_min, t.tm_sec);
}

bool locked() {
  return isLocked;
}

void wake() {
  lastInteraction = millis();
  unlock();
}

void lock() {
  if (isLocked) return;
  lastLockToggle = millis();
  isLocked = true;
  nextLockedWake = millis() + cfg.lockedWakeSeconds * 1000;
  if (!cfg.bluetoothWhileLocked) ble::stop();  // the phone disconnects; back on at unlock
  const String playing = nowPlaying();
  LOGI("ui", "locked%s", playing.length() ? " (keeps playing)" : "");
  drawLock();
  refresh(true);
}

}  // namespace shell
