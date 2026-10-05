#pragma once

#include <Arduino.h>

// FT6336 capacitive touch controller. The INT line is latched by an interrupt,
// so a tap is not lost while the main loop is blocked in an e-paper refresh.
class Touch {
 public:
  enum class Gesture { None, Tap, LongPress, SwipeUp, SwipeDown, SwipeLeft, SwipeRight };

  bool begin();

  // Hibernate (~µA) while the screen is locked; wake() resets the controller.
  void sleep();
  bool wake();

  // Call often. Tap and swipes fire on release (a swipe = the finger moved at least
  // kSwipePx, mostly in one direction; SwipeUp = moved up); LongPress fires once while
  // still held, if the finger hasn't moved.
  Gesture poll();

  static constexpr int16_t kSwipePx = 35;

  // Where the finger went down (panel coordinates, same as the display's).
  uint16_t x() const { return x_; }
  // Developer aid: the position the next "tap" reports (serial keys 1-9, see shell.cpp).
  void simulate(uint16_t x, uint16_t y) { x_ = x; y_ = y; }
  uint16_t y() const { return y_; }

 private:
  uint8_t touchCount();
  void readPoint(uint16_t &x, uint16_t &y);

  bool down_ = false;
  bool longReported_ = false;
  uint32_t downAt_ = 0;
  uint16_t x_ = 0;
  uint16_t y_ = 0;
  uint16_t lastX_ = 0;  // latest position while down
  uint16_t lastY_ = 0;
  bool moved_ = false;  // went past kSwipePx: not a tap or long press
};
