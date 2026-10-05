#include "touch.h"

#include <Wire.h>

#include "board_pins.h"
#include "log.h"

namespace {

constexpr uint32_t kLongPressMs = 900;

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

bool Touch::begin() {
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
  const bool sawInterrupt = interruptSeen;
  interruptSeen = false;
  const uint8_t count = touchCount();

  if (count > 0) {
    if (!down_) {
      down_ = true;
      longReported_ = false;
      moved_ = false;
      downAt_ = millis();
      readPoint(x_, y_);
      lastX_ = x_;
      lastY_ = y_;
    } else {
      readPoint(lastX_, lastY_);
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
  // A whole tap that started and ended while we were busy refreshing.
  return sawInterrupt ? Gesture::Tap : Gesture::None;
}

uint8_t Touch::touchCount() {
  uint8_t status = 0;
  if (readRegs(0x02, &status, 1) != 1) return 0;
  const uint8_t count = status & 0x0F;
  return count <= 2 ? count : 0;
}

void Touch::readPoint(uint16_t &x, uint16_t &y) {
  uint8_t buf[4];
  if (readRegs(0x03, buf, sizeof(buf)) != sizeof(buf)) return;
  x = (buf[0] & 0x0F) << 8 | buf[1];
  y = (buf[2] & 0x0F) << 8 | buf[3];
}
