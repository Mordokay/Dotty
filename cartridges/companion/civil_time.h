#pragma once

#include <stdint.h>

// Seconds since 1970 for a calendar date and time, with no zone: newlib (the ESP32's C library) has no
// timegm. Days from the civil date: Howard Hinnant's algorithm. Dotty keeps local time, so these are
// local epochs.
inline int64_t civilEpoch(int y, int m, int d, int h = 0, int mi = 0, int s = 0) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60 + s;
}
