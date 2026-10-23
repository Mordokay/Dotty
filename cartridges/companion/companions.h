#pragma once

#include <Arduino.h>

#include <string>

#include "companion_store.h"

// The companions on the SD card (spec: docs/superpowers/specs/2026-10-22-companion-roles-design.md):
// data/companions/<id>/{profile.json, memory/, history/, photo.bin}, data/companions/index.json,
// NVS companion/active. begin() moves piece 2a's memory and history into Debug's folder (once,
// resumable) and creates the five presets the first time. Selecting one points memories.* and
// history.* at its folder.
namespace companions {

void begin();  // after storage::begin()
const Profile &active();
const CompanionList &list();
const Profile *find(const std::string &id);
bool select(const std::string &id);
bool save(const Profile &p);  // replaces the profile with that id (the active one too)
bool create(const std::string &type, std::string &id);  // false when full
bool remove(const std::string &id);  // not the last one; the active one hands over to the next
String folder(const std::string &id);
// evolution.jsonl in the companion's folder: how it has changed (one JSON line per change or undo).
bool logEvolution(const std::string &id, const std::string &line);
std::string evolutionLog(const std::string &id);

}  // namespace companions
