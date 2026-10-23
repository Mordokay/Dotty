#pragma once

#include <Arduino.h>

#include <string>
#include <vector>

// Conversations on the card: data/history/YYYY-MM-DD.jsonl, a line per finished turn
// (usage_rules.h historyLine). The app reads them (companion.history).
namespace history {

// The active companion's folder: its history is <folder>/history/ (each companion has its own).
void setFolder(const String &companionFolder);
// memory: what this exchange changed in the memories ("remembered: …"), "" for nothing
void append(const std::string &you, const std::string &dotty, const std::string &mood, int ms, int tokens,
            const std::string &memory = "");
std::vector<String> days();       // "2026-10-22", newest first
String day(const String &yyyymmdd);  // that day's lines as a JSON array text ("[]" if none)
std::string raw(const String &yyyymmdd);  // that day's file as it is (.jsonl; "" if none)
bool forget(const String &yyyymmdd);  // "" = every day

}  // namespace history

// Today's use of the free allowance, counted on Dotty (Google has no API for it): turns and
// tokens since the last Pacific midnight, and "out of words until …" once Google refused.
namespace usage {

void addTurn(int tokens, int key = -1);  // key: which of the rotating keys (counted per key too)
void addTokens(int tokens, int key = -1);
int keyTurns(int key);   // today, per key
int keyTokens(int key);  // spent outside a question (interrupted or unasked replies)
int turnsToday();
int tokensToday();
void markOut(int64_t untilUtc);
int64_t outUntilUtc();  // 0 when not out

}  // namespace usage
