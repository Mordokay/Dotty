#include "battery.h"

#include "board_pins.h"

uint32_t batteryMillivolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_VBAT_ADC);
  return sum / 8 * 2;  // 1:2 divider
}

uint8_t batteryPercent(uint32_t mv) {
  struct Point {
    uint16_t mv;
    uint8_t percent;
  };
  static const Point kCurve[] = {
      {3300, 0},  {3600, 5},  {3700, 15}, {3750, 25}, {3800, 40},
      {3850, 55}, {3900, 65}, {4000, 80}, {4100, 92}, {4180, 100},
  };
  if (mv <= kCurve[0].mv) return 0;
  for (size_t i = 1; i < sizeof(kCurve) / sizeof(kCurve[0]); i++) {
    if (mv < kCurve[i].mv) {
      const Point &a = kCurve[i - 1], &b = kCurve[i];
      return a.percent + (mv - a.mv) * (b.percent - a.percent) / (b.mv - a.mv);
    }
  }
  return 100;
}
