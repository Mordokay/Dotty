#pragma once

#include "memory_store.h"

// The companion's memories on the SD card: /cartridges/companion/data/memory/memories.json,
// written through memories.json.part (a power cut keeps the old file); a file that doesn't parse
// is kept as memories.bad.json and an empty store started (chats work without memories).
namespace memories {

void begin();          // after storage::begin()
MemoryStore &store();
bool save();           // after every change
bool available();      // the card is there (else every memory command says "unavailable")

}  // namespace memories
