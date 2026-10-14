#pragma once

#include "shell.h"

// Touch calibration (see touch.h): rounds of 15 crosses. After each round Dotty fits a line per
// axis to every tap so far (raw reading against where the cross is), uses it straight away so the
// next round's dots land closer, and says how far off the round was. Again / Save; Save keeps it
// in NVS for every firmware. The first step of the welcome flow (an uncalibrated Dotty), and
// later a long press on the home screen or the app's Settings (touch.calibrate).
namespace calibrate {

void open(bool onboarding);
bool active();
// While active: handles this pass's input. Returns true when the screen needs drawing.
bool loop(const shell::Input &input);
void draw();

}  // namespace calibrate
