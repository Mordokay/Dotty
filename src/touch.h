#pragma once

#include <Arduino.h>

// FT6336 capacitive touch controller. The INT line is latched by an interrupt,
// so a tap is not lost while the main loop is blocked in an e-paper refresh.
class Touch {
 public:
  enum class Gesture { None, Tap, LongPress };

  bool begin();

  // Call often. Tap fires on release; LongPress fires once while still held.
  Gesture poll();

  // Where the finger went down (panel coordinates, same as the display's).
  uint16_t x() const { return x_; }
  uint16_t y() const { return y_; }

 private:
  uint8_t touchCount();
  void readPoint();

  bool down_ = false;
  bool longReported_ = false;
  uint32_t downAt_ = 0;
  uint16_t x_ = 0;
  uint16_t y_ = 0;
};
