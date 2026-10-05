#include "climate.h"

#include <Wire.h>

namespace climate {
namespace {

constexpr uint8_t kAddress = 0x70;

bool command(uint16_t cmd) {
  Wire.beginTransmission(kAddress);
  Wire.write(cmd >> 8);
  Wire.write(cmd & 0xFF);
  return Wire.endTransmission() == 0;
}

// CRC-8, polynomial 0x31, initial 0xFF (Sensirion).
uint8_t crc8(const uint8_t *data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = crc & 0x80 ? (crc << 1) ^ 0x31 : crc << 1;
  }
  return crc;
}

}  // namespace

bool read(Reading &out) {
  if (!command(0x3517)) return false;  // wake up
  delayMicroseconds(300);
  if (!command(0x7866)) return false;  // measure, temperature first, normal mode, no clock stretching
  delay(15);
  uint8_t r[6];
  if (Wire.requestFrom(kAddress, static_cast<uint8_t>(6)) != 6) return false;
  for (uint8_t &b : r) b = Wire.read();
  command(0xB098);  // sleep (~0.3 µA)
  if (crc8(r, 2) != r[2] || crc8(r + 3, 2) != r[5]) return false;
  out.celsius = -45.0f + 175.0f * ((r[0] << 8) | r[1]) / 65536.0f;
  out.humidity = 100.0f * ((r[3] << 8) | r[4]) / 65536.0f;
  return true;
}

}  // namespace climate
