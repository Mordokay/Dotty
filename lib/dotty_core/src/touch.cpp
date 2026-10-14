#include "touch.h"

#include <Preferences.h>
#include <Wire.h>

#include "board_pins.h"
#include "log.h"

namespace {

constexpr uint32_t kLongPressMs = 900;

float poly(const float *c, float u, float v) {
  return c[0] + c[1] * u + c[2] * v + c[3] * u * v + c[4] * u * u + c[5] * v * v;
}

constexpr uint32_t kCalibrationMagic = 0x54430002;  // "TC" v2: the quadratic

volatile bool interruptSeen = false;

void IRAM_ATTR onTouchInterrupt() {
  interruptSeen = true;
}

uint8_t readRegs(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(I2C_ADDR_FT6336);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  const uint8_t n = Wire.requestFrom(static_cast<uint8_t>(I2C_ADDR_FT6336), len);
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return n;
}

}  // namespace

void Touch::setCalibration(const Calibration &cal, bool save) {
  cal_ = cal;
  if (!save) return;
  Preferences p;
  p.begin("touch", false);
  p.clear();  // the straight-line keys of the first version
  p.putULong("magic", kCalibrationMagic);
  p.putBytes("cal", &cal_, sizeof(cal_));
  p.end();
  calibrated_ = true;
  LOGI("touch", "calibration saved: x = %.2f %+.2fu %+.2fv %+.2fuv %+.2fu2 %+.2fv2, y = %.2f %+.2fu %+.2fv %+.2fuv %+.2fu2 %+.2fv2",
       cal.x[0], cal.x[1], cal.x[2], cal.x[3], cal.x[4], cal.x[5], cal.y[0], cal.y[1], cal.y[2], cal.y[3], cal.y[4], cal.y[5]);
}

void Touch::toScreen(const Calibration &cal, uint16_t rawX, uint16_t rawY, float &x, float &y) {
  const float u = (rawX - 100.0f) / 100, v = (rawY - 100.0f) / 100;
  x = poly(cal.x, u, v);
  y = poly(cal.y, u, v);
  // At the clamp: halfway between where the clamp starts and the screen's edge.
  if (rawX == 0) x = max(0.0f, poly(cal.x, -1.0f, v)) / 2;
  if (rawX >= kRawMax) x = (min(float(kRawMax), poly(cal.x, (kRawMax - 100.0f) / 100, v)) + kRawMax) / 2;
  if (rawY == 0) y = max(0.0f, poly(cal.y, u, -1.0f)) / 2;
  if (rawY >= kRawMax) y = (min(float(kRawMax), poly(cal.y, u, (kRawMax - 100.0f) / 100)) + kRawMax) / 2;
}

void Touch::bands(const Calibration &cal, float &left, float &right) {
  left = poly(cal.x, -1.0f, 0);
  right = poly(cal.x, (kRawMax - 100.0f) / 100, 0);
}

bool Touch::begin() {
  Preferences p;
  if (p.begin("touch", true)) {
    Calibration saved;
    if (p.getULong("magic", 0) == kCalibrationMagic && p.getBytes("cal", &saved, sizeof(saved)) == sizeof(saved)) {
      cal_ = saved;
      calibrated_ = true;
    }
    p.end();
  }
  pinMode(PIN_TP_INT, INPUT_PULLUP);
  attachInterrupt(PIN_TP_INT, onTouchInterrupt, FALLING);
  return wake();
}

void Touch::sleep() {
  Wire.beginTransmission(I2C_ADDR_FT6336);
  Wire.write(0xA5);  // power mode
  Wire.write(0x03);  // hibernate
  Wire.endTransmission();
  down_ = false;
}

bool Touch::wake() {
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, LOW);
  delay(20);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(300);
  interruptSeen = false;

  // Probe the touch-status register; this board's controller NACKs the ID registers (0xA8).
  uint8_t status = 0;
  for (int attempt = 0; attempt < 5; attempt++) {
    if (readRegs(0x02, &status, 1) == 1) return true;
    delay(50);
  }
  LOGE("touch", "FT6336 not responding");
  return false;
}

Touch::Gesture Touch::poll() {
  interruptSeen = false;
  const uint8_t count = touchCount();

  if (count > 0) {
    if (!down_) {
      down_ = true;
      longReported_ = false;
      moved_ = false;
      downAt_ = millis();
      readPoint(x_, y_, true);
      lastX_ = x_;
      lastY_ = y_;
    } else {
      readPoint(lastX_, lastY_, false);
      if (abs(lastX_ - x_) >= kSwipePx || abs(lastY_ - y_) >= kSwipePx) moved_ = true;
      if (!moved_ && !longReported_ && millis() - downAt_ >= kLongPressMs) {
        longReported_ = true;
        return Gesture::LongPress;
      }
    }
    return Gesture::None;
  }

  if (down_) {
    down_ = false;
    if (longReported_) return Gesture::None;
    if (!moved_) return Gesture::Tap;
    const int16_t dx = lastX_ - x_, dy = lastY_ - y_;
    if (abs(dy) >= abs(dx)) return dy < 0 ? Gesture::SwipeUp : Gesture::SwipeDown;
    return dx < 0 ? Gesture::SwipeLeft : Gesture::SwipeRight;
  }
  // A whole touch began and ended between two polls (the loop was busy): we don't know where
  // it was or whether it was a swipe, so it's dropped. It used to count as a tap at the last
  // touch's place: a swipe down the launcher's slow cartridge list opened a cartridge.
  return Gesture::None;
}

uint8_t Touch::touchCount() {
  uint8_t status = 0;
  if (readRegs(0x02, &status, 1) != 1) return 0;
  const uint8_t count = status & 0x0F;
  return count <= 2 ? count : 0;
}

void Touch::readPoint(uint16_t &x, uint16_t &y, bool first) {
  uint8_t buf[4];
  if (readRegs(0x03, buf, sizeof(buf)) != sizeof(buf)) return;
  const uint16_t rawX = (buf[0] & 0x0F) << 8 | buf[1], rawY = (buf[2] & 0x0F) << 8 | buf[3];
  if (first) rawX_ = rawX, rawY_ = rawY;  // where it went down, like x()/y()
  float sx, sy;
  toScreen(cal_, rawX, rawY, sx, sy);
  x = constrain(lroundf(sx), 0, kRawMax);
  y = constrain(lroundf(sy), 0, kRawMax);
}
