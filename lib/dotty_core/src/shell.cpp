#include "shell.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Wire.h>

#include "battery.h"
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

constexpr uint32_t kPowerOffHoldMs = 2000;
constexpr uint32_t kLauncherComboMs = 1000;
constexpr uint32_t kAutoLockMs = 2 * 60 * 1000;
constexpr uint32_t kBatteryLogIntervalMs = 10 * 60 * 1000;
constexpr uint32_t kPowerCardMs = 2500;
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

void drawLock() {
  LockScreenInfo info = {};
  info.timeValid = rtc.read(info.time);
  info.batteryPercent = battery::percent();
  info.externalPower = battery::external();
  info.charging = battery::charging();
  info.batteryLow = battery::low();
  info.nowPlaying = nowPlaying();
  drawLockScreen(epd, info);
  lockShownMinute = info.time.tm_min;
  lockShownText = info.nowPlaying;
  lockShownBattery = batteryKey();
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
  wakePeripherals();
  ble::start();
  isLocked = false;
  lastInteraction = millis();
  LOGI("ui", "unlocked");
  showApp();
}

[[noreturn]] void powerOff() {
  LOGI("power", "power off");
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
  const bool pwr = digitalRead(PIN_BTN_PWR) == LOW;
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

  if (!pwr) {
    if (pwrArmed && pwrPressedAt != 0 && !pwrLongHandled && !comboUsed) {
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
  const bool canSleep = power::wakeLocks() == 0 && !power::usbHostConnected();

  if (!canSleep) {
    if (millis() - lastCheck < 1000 || epd.isBusy()) return;
    lastCheck = millis();
    tm now;
    rtc.read(now);
    const bool appChanged = runLockedWake();
    if (appChanged || now.tm_min != lockShownMinute || nowPlaying() != lockShownText ||
        batteryKey() != lockShownBattery) {
      drawLock();
      refresh(false);
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
    drawLock();
    refresh(false);
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
  const bool byButton = power::lightSleep(sleepMs);
  power::setAudioRail(true);
  delay(2);
  if (byButton) {
    LOGI("power", "woken by PWR");
    pwrArmed = false;  // this press unlocks; its release must not lock again
    unlock();
  }
}

// While an iPhone pairs, Dotty shows the code to type on the phone.
void drawPairing(uint32_t code) {
  char digits[8];
  snprintf(digits, sizeof(digits), "%03lu %03lu", code / 1000, code % 1000);
  epd.fillScreen(EpdDisplay::kWhite);
  ui::drawHeader(epd, "Pair with iPhone");
  epd.setTextColor(EpdDisplay::kBlack);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, "Type this code", 70);
  ui::drawCentered(epd, "on your iPhone", 90);
  epd.setFont(&FreeSansBold24pt7b);
  ui::drawCentered(epd, digits, 145);
}

void drawPairingResult(bool success) {
  epd.fillScreen(EpdDisplay::kWhite);
  ui::drawHeader(epd, "Pair with iPhone");
  epd.setTextColor(EpdDisplay::kBlack);
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, success ? "Paired!" : "Pairing failed", 110);
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
  lastInteraction = millis();
}

bool update(Input &input) {
  battery::poll();
  if (battery::empty()) batteryEmptyOff();
  logBattery();
  ble::poll();
  handlePairing();
  power::setWakeLock(power::kWakeLockBle, ble::connected());
  const bool bootClick = handleButtons();
  const Touch::Gesture gesture = touch.poll();

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
    LOGI("touch", "%s at (%u, %u)", gesture == Touch::Gesture::Tap ? "tap" : "long press",
         touch.x(), touch.y());
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

bool locked() {
  return isLocked;
}

void wake() {
  lastInteraction = millis();
  unlock();
}

void lock() {
  if (isLocked) return;
  isLocked = true;
  nextLockedWake = millis() + cfg.lockedWakeSeconds * 1000;
  if (!cfg.bluetoothWhileLocked) ble::stop();  // the phone disconnects; back on at unlock
  const String playing = nowPlaying();
  LOGI("ui", "locked%s", playing.length() ? " (keeps playing)" : "");
  drawLock();
  refresh(true);
}

}  // namespace shell
