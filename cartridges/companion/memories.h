#pragma once

#include "memory_store.h"

// A companion's memories on the SD card: <its folder>/memory/memories.json,
// written through memories.json.part (a power cut keeps the old file); a file that doesn't parse
// is kept as memories.bad.json and an empty store started (chats work without memories).
namespace memories {

// Loads a companion's memories (<companionFolder>/memory/memories.json); called on every switch.
void begin(const String &companionFolder);
MemoryStore &store();
bool save();           // after every change
bool available();      // the card is there (else every memory command says "unavailable")

}  // namespace memories
