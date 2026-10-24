#pragma once

#include <Arduino.h>

#include <string>
#include <vector>

#include "moment_schedule.h"
#include "moment_store.h"
#include "timer_beeps.h"

// Scheduled moments on the card (spec: docs/superpowers/specs/2026-10-24-companion-moments-design.md):
// each companion's data/companions/<id>/moments.json and its recorded lines in moments/<key>.wav + .json
// (key = the moment's id, "group-<minute>" for several at one minute, or "timer"), the rules shared by
// every companion (data/moment_rules.json), and the one timer (NVS companion timer*: a restart keeps it).
namespace moments {

void begin();  // after companions::begin(): loads everything, deletes recordings no moment needs
bool available();  // the card is there (a running timer works without it)
std::vector<CompanionMoments> all();  // in the companions' order
MomentList *of(const std::string &companion);  // nullptr: no such companion (or no card)
bool save(const std::string &companion);
MomentRules &rules();
bool saveRules();
TimerState &timer();
void saveTimer();
String recordingPath(const std::string &companion, const std::string &key, const char *ext);  // "wav" or "json"
// The recording exists and was made for this signature (its .json's "signature").
bool recorded(const std::string &companion, const std::string &key, const std::string &signature);
void dropRecording(const std::string &companion, const std::string &key);
void companionRemoved(const std::string &companion);  // its folder goes with it

}  // namespace moments
