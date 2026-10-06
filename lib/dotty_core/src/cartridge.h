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

#include <esp_partition.h>

namespace cartridge {

// Flash layout 2 (partitions.csv): Rescue in factory, the launcher in ota_1, the cartridge
// in ota_0.
const esp_partition_t *launcherSlot();
const esp_partition_t *rescueSlot();

// The running firmware's identity.
const CartridgeInfo &self();

// True when running from the launcher slot.
bool isLauncher();

// Reads the identity of the cartridge in ota_0; false if none is installed.
bool readInstalled(CartridgeInfo &out);

// Reads the launcher's identity (its slot), e.g. its version for the app.
bool readLauncher(CartridgeInfo &out);

// A firmware started for the first time is on trial: if it restarts before confirming,
// the bootloader rolls back to the previous one. The shell confirms after a few seconds of
// running (and before any deliberate restart). True if this call confirmed it.
bool confirmHealthy();

// The launcher slot holds a launcher that failed its trial (or nothing valid): Rescue
// should put a good one back.
bool launcherBroken();

// Empties ota_0 (erases its image header) and boots the launcher from now on. Used when the
// installed cartridge is removed.
bool eraseInstalled();

// Boot targets. They restart the chip and do not return on success.
void rebootToLauncher();
void rebootToRescue();
bool startInstalled();

}  // namespace cartridge
