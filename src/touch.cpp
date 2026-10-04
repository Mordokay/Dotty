#include "touch.h"

#include <Wire.h>

#include "board_pins.h"

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
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, LOW);
  delay(20);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(300);

  pinMode(PIN_TP_INT, INPUT_PULLUP);
  attachInterrupt(PIN_TP_INT, onTouchInterrupt, FALLING);

  // Probe the touch-status register; this board's controller NACKs the ID registers (0xA8).
  uint8_t status = 0;
  for (int attempt = 0; attempt < 5; attempt++) {
    if (readRegs(0x02, &status, 1) == 1) return true;
    delay(50);
  }
  log_e("FT6336 not responding");
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
      downAt_ = millis();
      readPoint();
    } else if (!longReported_ && millis() - downAt_ >= kLongPressMs) {
      longReported_ = true;
      return Gesture::LongPress;
    }
    return Gesture::None;
  }

  if (down_) {
    down_ = false;
    return longReported_ ? Gesture::None : Gesture::Tap;
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

void Touch::readPoint() {
  uint8_t buf[4];
  if (readRegs(0x03, buf, sizeof(buf)) != sizeof(buf)) return;
  x_ = (buf[0] & 0x0F) << 8 | buf[1];
  y_ = (buf[2] & 0x0F) << 8 | buf[3];
}
