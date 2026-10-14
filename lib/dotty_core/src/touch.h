#pragma once

#include <Arduino.h>

// FT6336 capacitive touch controller. Gestures come from polling: a touch that begins and ends
// while the loop is busy is dropped (see poll()).
//
// This panel's readings don't line up with the screen: x is stretched ~1.4x out from the
// middle and both axes bend a little, and the chip clamps its readings to 0..199, so the outer
// ~27 px on the left and ~30 px on the right all read the same. The correction maps a raw
// reading to the screen with a quadratic in both raw coordinates per axis (a straight line per
// axis left ~3 px it couldn't fix), fitted on Dotty by the launcher's calibration screen and
// kept in NVS (namespace "touch"), for every firmware.
class Touch {
 public:
  enum class Gesture { None, Tap, LongPress, SwipeUp, SwipeDown, SwipeLeft, SwipeRight };

  // screen = c[0] + c[1] u + c[2] v + c[3] u v + c[4] u^2 + c[5] v^2 per axis, with
  // u = (rawX - 100) / 100 and v = (rawY - 100) / 100.
  static constexpr int kTerms = 6;
  struct Calibration {
    // The prototype's own calibration (2026-10-14, 3 rounds; its typing test then had 1 slip,
    // in the right blind strip), used until a Dotty is calibrated or when the welcome flow's
    // calibration is skipped.
    float x[kTerms] = {94.60f, 70.67f, -1.03f, -1.04f, 1.35f, 10.87f};
    float y[kTerms] = {94.16f, -1.68f, 117.65f, 0.30f, 0.37f, -15.11f};
  };
  static constexpr uint16_t kRawMax = 199;  // the chip's readings end here
  // The calibration in use; calibrated() = one was saved (a calibration screen was finished).
  const Calibration &calibration() const { return cal_; }
  bool calibrated() const { return calibrated_; }
  void setCalibration(const Calibration &cal, bool save);
  // Screen position for a raw reading through `cal` (unclamped, unrounded). A reading at the
  // chip's clamp (0 or kRawMax) stands for that edge's whole band: its middle.
  static void toScreen(const Calibration &cal, uint16_t rawX, uint16_t rawY, float &x, float &y);
  // Where the left and right blind strips begin on the screen (x at the middle height): every
  // touch left of `left` reads as one position, every touch right of `right` as another.
  static void bands(const Calibration &cal, float &left, float &right);
  // The raw reading behind the last x()/y() (for calibrating).
  uint16_t rawX() const { return rawX_; }
  uint16_t rawY() const { return rawY_; }

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
  void readPoint(uint16_t &x, uint16_t &y, bool first);

  Calibration cal_;
  bool calibrated_ = false;
  uint16_t rawX_ = 0, rawY_ = 0;

  bool down_ = false;
  bool longReported_ = false;
  uint32_t downAt_ = 0;
  uint16_t x_ = 0;
  uint16_t y_ = 0;
  uint16_t lastX_ = 0;  // latest position while down
  uint16_t lastY_ = 0;
  bool moved_ = false;  // went past kSwipePx: not a tap or long press
};
