#pragma once

#include <Adafruit_GFX.h>

// 1.54" 200x200 black/white e-paper (SSD1681-class controller).
// Init sequence and waveform LUTs are taken from Waveshare's example driver.
//
// Draw into the in-memory canvas with the Adafruit_GFX API (print, drawLine,
// fillRect, setFont, ...), then push it to the panel with one of the refreshes.
class EpdDisplay : public GFXcanvas1 {
 public:
  static constexpr int16_t kSize = 200;
  static constexpr uint16_t kWhite = 1;
  static constexpr uint16_t kBlack = 0;

  EpdDisplay();

  // Powers the panel and sets up SPI. Call once before any refresh.
  void begin();

  // Full refresh: the panel flashes black/white for ~2 s and all ghosting is cleared.
  // Also stores the image as the base for later partial refreshes.
  void refreshFull();

  // Partial refresh: no flash, well under a second. Ghosting slowly builds up,
  // so do a refreshFull() every few dozen partial updates.
  void refreshPartial();

  // Cuts power to the panel. The image stays on screen.
  void powerOff();

 private:
  void reset();
  void initFull();
  void initPartial();
  void setRamArea();
  void setLut(const uint8_t *lut);
  void writeRam(uint8_t command);
  void update(uint8_t mode);
  void waitBusy();
  void command(uint8_t cmd);
  void data(uint8_t value);
  void data(const uint8_t *values, size_t len);

  bool partialMode_ = false;
};
