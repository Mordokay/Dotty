#pragma once

#include "shell.h"

// Joining a Wi-Fi network on Dotty itself, without the phone: a list of the networks in
// range, then a keyboard for the password (20x20 px keys, 6 rows under a password field).
// Opened from the Cartridges screen's "Wi-Fi" row, or by tapping the welcome screen that asks
// for Wi-Fi. Joining checks the password first and saves the network only if it worked, like
// the app's wifi.add.
namespace wifisetup {

void open();
bool active();
// While active: handles this pass's input. Returns true when the screen needs drawing.
bool loop(const shell::Input &input);
void draw();

}  // namespace wifisetup
