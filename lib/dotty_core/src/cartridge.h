#pragma once

#include <Arduino.h>

// Identity of a firmware image. Every firmware (the launcher too) declares one with
// DOTTY_CARTRIDGE(...) in its main.cpp. The linker places it in .rodata_custom_desc,
// right after ESP-IDF's app description, at a fixed offset in the image, so the
// launcher can read which cartridge is installed straight from flash.
struct CartridgeInfo {
  uint32_t magic;
  char id[16];       // "music"
  char name[24];     // "Music"
  char version[16];  // "0.5.0"
};

constexpr uint32_t kCartridgeMagic = 0x59544F44;  // "DOTY"

#define DOTTY_CARTRIDGE(ID, NAME, VERSION)                                                  \
  extern "C" const CartridgeInfo kDottyCartridge                                            \
      __attribute__((section(".rodata_custom_desc"), used)) = {kCartridgeMagic, ID, NAME, VERSION}

namespace cartridge {

// The running firmware's identity.
const CartridgeInfo &self();

// True when running from the factory partition.
bool isLauncher();

// Reads the identity of the cartridge in ota_0; false if none is installed.
bool readInstalled(CartridgeInfo &out);

// Boot targets. Both restart the chip and do not return on success.
void rebootToLauncher();
bool startInstalled();

}  // namespace cartridge
