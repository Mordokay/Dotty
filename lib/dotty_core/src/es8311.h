#pragma once

#include <Wire.h>

// Minimal ES8311 codec driver: DAC playback, plus the analog microphone through the ADC,
// codec as I2S slave clocked by MCLK = 256 x sample rate. Register sequence follows
// Espressif's esp_codec_dev es8311.c (open -> set_fs -> start).
class Es8311 {
 public:
  bool begin(TwoWire &wire, uint8_t address, uint32_t sampleRate);
  void setSampleRate(uint32_t sampleRate);

  // 0 = silent, 1..100 = -40..+6 dB.
  void setVolume(uint8_t percent);
  void setMute(bool mute);

  // Microphone samples on the ADC serial port (I2S DIN), with analog gain 0..42 dB (6 dB
  // steps) and a digital volume on top (0.5 dB steps, up to +32). Off = the port is muted
  // (begin() leaves it off).
  void setMicrophone(bool on, uint8_t gainDb = 30, float digitalDb = 0);

 private:
  void write(uint8_t reg, uint8_t value);
  uint8_t read(uint8_t reg);

  TwoWire *wire_ = nullptr;
  uint8_t address_ = 0;
};
