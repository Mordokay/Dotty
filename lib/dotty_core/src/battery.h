#pragma once

#include <Arduino.h>

// Battery state from the voltage on GPIO4 (1:2 divider). The board has no fuel gauge, and
// the charger's status pin only lights the orange LED (nothing reaches the ESP32), so:
//  - The percentage comes from a LiPo voltage curve, smoothed, and on battery it only goes
//    down when a lower reading lasts a minute: the voltage sags under load (music, Wi-Fi,
//    screen refreshes), and in the flat middle of the curve 30 mV is ~9 %.
//  - External power is inferred. A computer shows up over USB. A charger shows as the
//    voltage jumping up: on USB power the system runs from VBUS (Q5 cuts the battery off
//    the system) and the battery is charged, so its voltage rises at once; it drops again
//    when unplugged.
namespace battery {

constexpr uint8_t kLowPercent = 10;
constexpr uint32_t kEmptyMv = 3400;  // below this on battery: show "Battery empty", power off

void begin();
// Samples every few seconds (every wake while locked). Call from the main loop.
void poll();

uint8_t percent();         // what to show
uint32_t millivolts();     // smoothed
bool external();           // a charger or computer is powering Dotty
bool charging();           // external power and not full yet
bool low();                // on battery, at or below kLowPercent
bool empty();              // on battery and below kEmptyMv for a while

// A fresh reading, for logs.
uint32_t readMillivolts();

}  // namespace battery
