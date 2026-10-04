#include "es8311.h"

#include <Arduino.h>

namespace {

constexpr uint8_t kRegReset = 0x00;
constexpr uint8_t kRegClk01 = 0x01;
constexpr uint8_t kRegClk02 = 0x02;
constexpr uint8_t kRegClk03 = 0x03;
constexpr uint8_t kRegClk04 = 0x04;
constexpr uint8_t kRegClk05 = 0x05;
constexpr uint8_t kRegClk06 = 0x06;
constexpr uint8_t kRegClk07 = 0x07;
constexpr uint8_t kRegClk08 = 0x08;
constexpr uint8_t kRegSdpIn = 0x09;   // DAC serial port
constexpr uint8_t kRegSdpOut = 0x0A;  // ADC serial port
constexpr uint8_t kRegDacMute = 0x31;
constexpr uint8_t kRegDacVolume = 0x32;
constexpr uint8_t kRegChipId = 0xFD;

}  // namespace

bool Es8311::begin(TwoWire &wire, uint8_t address, uint32_t sampleRate) {
  wire_ = &wire;
  address_ = address;

  const uint8_t id = read(kRegChipId);
  if (id != 0x83) {
    log_e("ES8311 not found (chip id 0x%02X)", id);
    return false;
  }

  // open
  write(0x44, 0x08);  // I2C noise immunity; written twice, the first write can fail
  write(0x44, 0x08);
  write(kRegClk01, 0x30);
  write(kRegClk02, 0x00);
  write(kRegClk03, 0x10);
  write(0x16, 0x24);
  write(kRegClk04, 0x10);
  write(kRegClk05, 0x00);
  write(0x0B, 0x00);
  write(0x0C, 0x00);
  write(0x10, 0x1F);
  write(0x11, 0x7F);
  write(kRegReset, 0x80);  // power on, slave mode
  write(kRegClk01, 0x3F);  // clocks from MCLK pin, not inverted
  write(kRegClk06, read(kRegClk06) & ~0x20);
  write(0x13, 0x10);
  write(0x1B, 0x0A);
  write(0x1C, 0x6A);
  write(0x44, 0x58);

  // set_fs: 16-bit Philips I2S on both ports
  write(kRegSdpIn, (read(kRegSdpIn) & 0xFC) | 0x0C);
  write(kRegSdpOut, (read(kRegSdpOut) & 0xFC) | 0x0C);
  setSampleRate(sampleRate);

  // start: DAC enabled, ADC port muted
  write(kRegReset, 0x80);
  write(kRegClk01, 0x3F);
  write(kRegSdpIn, read(kRegSdpIn) & ~0x40);
  write(kRegSdpOut, read(kRegSdpOut) | 0x40);
  write(0x17, 0xBF);
  write(0x0E, 0x02);
  write(0x12, 0x00);
  write(0x14, 0x1A);
  write(0x0D, 0x01);
  write(0x15, 0x40);
  write(0x37, 0x08);
  write(0x45, 0x00);
  return true;
}

// Coefficients for MCLK = 256 x fs: no pre-divide/multiply, LRCK = MCLK / 256.
void Es8311::setSampleRate(uint32_t sampleRate) {
  const uint8_t dacOsr = sampleRate <= 16000 ? 0x20 : 0x10;
  write(kRegClk02, read(kRegClk02) & 0x07);
  write(kRegClk05, 0x00);
  write(kRegClk03, (read(kRegClk03) & 0x80) | 0x10);
  write(kRegClk04, (read(kRegClk04) & 0x80) | dacOsr);
  write(kRegClk07, read(kRegClk07) & 0xC0);
  write(kRegClk08, 0xFF);
  write(kRegClk06, (read(kRegClk06) & 0xE0) | 0x03);
}

// Register 0x32: 0x00 = -95.5 dB, 0xBF = 0 dB, 0.5 dB steps. Map 1-100 % onto -40..+6 dB:
// the onboard speaker is quiet, so the top of the range adds digital gain (loud tracks may clip).
void Es8311::setVolume(uint8_t percent) {
  if (percent > 100) percent = 100;
  uint8_t reg = 0;
  if (percent > 0) {
    const float db = -40.0f + 46.0f * percent / 100.0f;
    reg = static_cast<uint8_t>((db + 95.5f) * 2.0f);
  }
  write(kRegDacVolume, reg);
}

void Es8311::setMute(bool mute) {
  const uint8_t value = read(kRegDacMute) & 0x9F;
  write(kRegDacMute, mute ? value | 0x60 : value);
}

void Es8311::write(uint8_t reg, uint8_t value) {
  wire_->beginTransmission(address_);
  wire_->write(reg);
  wire_->write(value);
  wire_->endTransmission();
}

uint8_t Es8311::read(uint8_t reg) {
  wire_->beginTransmission(address_);
  wire_->write(reg);
  wire_->endTransmission(false);
  if (wire_->requestFrom(address_, static_cast<uint8_t>(1)) != 1) return 0;
  return wire_->read();
}
