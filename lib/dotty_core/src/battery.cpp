#include "battery.h"

#include "board_pins.h"
#include "log.h"
#include "power.h"

namespace battery {
namespace {

constexpr uint32_t kSampleMs = 5000;
// A step this big between two samples means a charger was plugged in or pulled out
// (load changes are ignored for a while, see kLoadSettleMs).
constexpr int32_t kPlugJumpMv = 40;
constexpr uint32_t kLoadSettleMs = 15000;
// Charging voltage sits above rest: show the charge from a shifted curve, and only call
// the battery full once it has held the charger's top voltage for a while.
constexpr uint32_t kChargeOffsetMv = 80;
constexpr uint32_t kFullMv = 4170;
constexpr uint32_t kFullAfterMs = 10 * 60 * 1000;
constexpr uint32_t kSurelyExternalMv = 4200;  // only a charger holds the battery this high
constexpr uint32_t kDropAfterMs = 60 * 1000;   // on battery: a lower reading must last this long
constexpr uint32_t kEmptyAfterMs = 60 * 1000;
// Unplugging rarely shows as one step: the voltage relaxes over minutes. A battery that is
// really charging keeps rising (or holds near the charger's 4.2 V once full), so external
// power that stops rising while below charging level has been unplugged.
constexpr uint32_t kRiseWindowMs = 5 * 60 * 1000;
constexpr int32_t kRiseMv = 8;
constexpr uint32_t kChargingLevelMv = 4150;  // charging, the battery reads above this soon
constexpr uint32_t kFullFloorMv = 4090;      // the charger tops up again before it gets here

float smoothed = 0;
uint32_t lastRaw = 0;
uint32_t lastSample = 0;
uint32_t lastWakeLocks = 0;
uint32_t lastLoadChange = 0;
bool onExternal = false;
uint32_t externalSince = 0;
uint32_t fullSince = 0;
bool isFull = false;
uint8_t shown = 0;
uint32_t lowerSince = 0;
uint32_t emptySince = 0;
float riseRef = 0;       // smoothed voltage at the start of the current rise window
uint32_t riseSince = 0;

uint8_t curve(int32_t mv) {
  struct Point {
    int16_t mv;
    uint8_t percent;
  };
  static const Point kCurve[] = {
      {3300, 0},  {3600, 5},  {3700, 15}, {3750, 25}, {3800, 40},
      {3850, 55}, {3900, 65}, {4000, 80}, {4100, 92}, {4180, 100},
  };
  if (mv <= kCurve[0].mv) return 0;
  for (size_t i = 1; i < sizeof(kCurve) / sizeof(kCurve[0]); i++) {
    if (mv < kCurve[i].mv) {
      const Point &a = kCurve[i - 1], &b = kCurve[i];
      return a.percent + (mv - a.mv) * (b.percent - a.percent) / (b.mv - a.mv);
    }
  }
  return 100;
}

void setExternal(bool on, uint32_t raw, const char *why) {
  if (on == onExternal) return;
  onExternal = on;
  externalSince = millis();
  fullSince = 0;
  isFull = false;
  smoothed = raw;  // the old level no longer applies
  riseRef = raw;
  riseSince = millis();
  LOGI("battery", "%s (%s, %lu mV)", on ? "external power" : "on battery", why, raw);
}

void updateShown(uint32_t now) {
  if (onExternal) {
    // Charging: rises with the (offset) charge, never jumps back down.
    const uint8_t target = isFull ? 100 : min<uint8_t>(99, curve(static_cast<int32_t>(smoothed) - kChargeOffsetMv));
    if (target > shown) shown = target;
    lowerSince = 0;
    return;
  }
  // On battery: go down one step at a time, and only for a lower level that lasts.
  const uint8_t target = curve(static_cast<int32_t>(smoothed));
  if (target < shown) {
    if (lowerSince == 0) lowerSince = now;
    if (now - lowerSince >= kDropAfterMs) {
      shown--;
      lowerSince = now;  // the next step needs another minute
    }
  } else {
    lowerSince = 0;
  }
}

}  // namespace

uint32_t readMillivolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_VBAT_ADC);
  return sum / 16 * 2;  // 1:2 divider
}

void begin() {
  lastRaw = readMillivolts();
  smoothed = lastRaw;
  lastSample = millis();
  lastWakeLocks = power::wakeLocks();
  onExternal = power::usbHostConnected() || lastRaw >= kSurelyExternalMv;
  externalSince = millis();
  shown = onExternal ? min<uint8_t>(99, curve(static_cast<int32_t>(lastRaw) - kChargeOffsetMv)) : curve(lastRaw);
  if (!onExternal && lastRaw < kEmptyMv) emptySince = millis() - kEmptyAfterMs;  // empty at boot
  LOGI("battery", "%lu mV, %u%%%s", lastRaw, shown, onExternal ? ", external power" : "");
}

void poll() {
  const uint32_t now = millis();
  // Music, Wi-Fi and the like move the voltage too: don't read those steps as a charger.
  const uint32_t locks = power::wakeLocks();
  if (locks != lastWakeLocks) {
    lastWakeLocks = locks;
    lastLoadChange = now;
  }
  if (now - lastSample < kSampleMs) return;
  lastSample = now;

  const uint32_t raw = readMillivolts();
  const int32_t step = static_cast<int32_t>(raw) - static_cast<int32_t>(lastRaw);
  lastRaw = raw;
  const bool loadSteady = now - lastLoadChange > kLoadSettleMs;
  const bool host = power::usbHostConnected();

  if (host) {
    setExternal(true, raw, "computer");
  } else if (!onExternal) {
    if (raw >= kSurelyExternalMv) setExternal(true, raw, "charge voltage");
    else if (step >= kPlugJumpMv && loadSteady) setExternal(true, raw, "voltage jumped up");
  } else if (step <= -kPlugJumpMv) {
    // On external power the system doesn't load the battery, so any sudden drop is the
    // charger going away (a computer is checked above).
    setExternal(false, raw, "voltage dropped");
  }

  smoothed = smoothed * 0.75f + raw * 0.25f;
  if (onExternal && !host) {
    // No longer charging? (A computer is trusted while it's attached.)
    if (smoothed >= riseRef + kRiseMv) {
      riseRef = smoothed;
      riseSince = now;
    } else if (now - riseSince >= kRiseWindowMs) {
      if (isFull ? smoothed < kFullFloorMv : smoothed < kChargingLevelMv) {
        setExternal(false, static_cast<uint32_t>(smoothed), "stopped rising");
      } else {
        riseRef = smoothed;  // holding at the top: still plugged in
        riseSince = now;
      }
    }
  }
  if (onExternal) {
    if (smoothed >= kFullMv) {
      if (fullSince == 0) fullSince = now;
      if (!isFull && now - fullSince >= kFullAfterMs) {
        isFull = true;
        LOGI("battery", "full");
      }
    } else {
      fullSince = 0;
    }
    emptySince = 0;
  } else if (smoothed < kEmptyMv) {
    if (emptySince == 0) emptySince = now;
  } else {
    emptySince = 0;
  }
  updateShown(now);
}

uint8_t percent() {
  return shown;
}

uint32_t millivolts() {
  return static_cast<uint32_t>(smoothed);
}

bool external() {
  return onExternal;
}

bool charging() {
  return onExternal && !isFull;
}

bool low() {
  return !onExternal && shown <= kLowPercent;
}

bool empty() {
  return !onExternal && emptySince != 0 && millis() - emptySince >= kEmptyAfterMs;
}

}  // namespace battery
