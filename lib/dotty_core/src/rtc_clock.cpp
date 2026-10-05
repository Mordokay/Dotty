#include "rtc_clock.h"

#include <Arduino.h>
#include <sys/time.h>

#include "board_pins.h"
#include "log.h"

namespace {

constexpr uint8_t kRegControl1 = 0x00;
constexpr uint8_t kRegSeconds = 0x04;  // bit 7: oscillator stopped (time invalid)

uint8_t toBcd(int v) {
  return ((v / 10) << 4) | (v % 10);
}

int fromBcd(uint8_t v) {
  return (v >> 4) * 10 + (v & 0x0F);
}

// Keeps the system clock (time(), file timestamps on the SD card) on the RTC's local time.
// The system clock drifts in light sleep (no 32 kHz crystal), so every read re-syncs it.
void syncSystemTime(const tm &local) {
  tm copy = local;
  const time_t t = mktime(&copy);
  if (labs(static_cast<long>(time(nullptr) - t)) > 2) {
    const timeval tv = {t, 0};
    settimeofday(&tv, nullptr);
  }
}

// __DATE__ = "Oct  4 2026", __TIME__ = "17:42:05"
tm buildTime() {
  static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char month[4] = {};
  tm t = {};
  sscanf(__DATE__, "%3s %d %d", month, &t.tm_mday, &t.tm_year);
  sscanf(__TIME__, "%d:%d:%d", &t.tm_hour, &t.tm_min, &t.tm_sec);
  t.tm_mon = (strstr(kMonths, month) - kMonths) / 3;
  t.tm_year -= 1900;
  mktime(&t);  // fills in the weekday
  return t;
}

}  // namespace

bool RtcClock::begin(TwoWire &wire) {
  wire_ = &wire;
  wire_->beginTransmission(I2C_ADDR_PCF85063);
  wire_->write(kRegControl1);
  wire_->write(0x00);  // running, 24 h mode
  if (wire_->endTransmission() != 0) {
    LOGE("rtc", "PCF85063 not responding");
    return false;
  }

  tm now = {};
  const bool valid = read(now);
  tm built = buildTime();
  if (!valid || mktime(&now) < mktime(&built)) {
    LOGW("rtc", "clock %s, setting it to build time", valid ? "behind" : "lost power");
    write(built);
  }
  return true;
}

bool RtcClock::read(tm &out) {
  wire_->beginTransmission(I2C_ADDR_PCF85063);
  wire_->write(kRegSeconds);
  if (wire_->endTransmission(false) != 0) return false;
  uint8_t r[7];
  if (wire_->requestFrom(static_cast<uint8_t>(I2C_ADDR_PCF85063), static_cast<uint8_t>(7)) != 7) return false;
  for (uint8_t &b : r) b = wire_->read();

  out = {};
  out.tm_sec = fromBcd(r[0] & 0x7F);
  out.tm_min = fromBcd(r[1] & 0x7F);
  out.tm_hour = fromBcd(r[2] & 0x3F);
  out.tm_mday = fromBcd(r[3] & 0x3F);
  out.tm_wday = r[4] & 0x07;
  out.tm_mon = fromBcd(r[5] & 0x1F) - 1;
  out.tm_year = fromBcd(r[6]) + 100;  // 2000-2099
  const bool valid = !(r[0] & 0x80);
  if (valid) syncSystemTime(out);
  return valid;
}

bool RtcClock::write(const tm &t) {
  wire_->beginTransmission(I2C_ADDR_PCF85063);
  wire_->write(kRegSeconds);
  wire_->write(toBcd(t.tm_sec));  // also clears the oscillator-stopped flag
  wire_->write(toBcd(t.tm_min));
  wire_->write(toBcd(t.tm_hour));
  wire_->write(toBcd(t.tm_mday));
  wire_->write(t.tm_wday);
  wire_->write(toBcd(t.tm_mon + 1));
  wire_->write(toBcd(t.tm_year - 100));
  const bool ok = wire_->endTransmission() == 0;
  if (ok) syncSystemTime(t);
  return ok;
}
