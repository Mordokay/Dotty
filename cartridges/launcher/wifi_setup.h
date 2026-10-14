#pragma once

#include "shell.h"

// Joining a Wi-Fi network on Dotty itself, without the phone: a list of the networks in
// range, then a keyboard for the password (8 x 4 keys of 25 x 31 px, letter / number / symbol
// pages). Opened from the Wi-Fi button in the home screen's top-left corner, or by tapping the
// welcome screen that asks for Wi-Fi. Joining checks the password first and saves the network
// only if it worked, like the app's wifi.add.
namespace wifisetup {

void open();
bool active();
// While active: handles this pass's input. Returns true when the screen needs drawing.
bool loop(const shell::Input &input);
void draw();

}  // namespace wifisetup
