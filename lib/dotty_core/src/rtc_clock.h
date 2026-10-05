#pragma once

#include <Wire.h>
#include <time.h>

// PCF85063 real-time clock. Stores local time (no time zone handling yet). Reads and
// writes also set the system clock to it, so time() and SD file dates are local time.
class RtcClock {
 public:
  // Starts the clock. If it lost power (or holds a time older than this
  // firmware's build), it is set to the build time until Wi-Fi/BLE can set it.
  bool begin(TwoWire &wire);

  bool read(tm &out);
  bool write(const tm &t);

 private:
  TwoWire *wire_ = nullptr;
};
