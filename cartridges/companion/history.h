#pragma once

#include <Arduino.h>

#include <string>
#include <vector>

// Conversations on the card: data/history/YYYY-MM-DD.jsonl, a line per finished turn
// (usage_rules.h historyLine). The app reads them (companion.history).
namespace history {

void append(const std::string &you, const std::string &dotty, const std::string &mood, int ms, int tokens);
std::vector<String> days();       // "2026-10-22", newest first
String day(const String &yyyymmdd);  // that day's lines as a JSON array text ("[]" if none)
bool forget(const String &yyyymmdd);  // "" = every day

}  // namespace history

// Today's use of the free allowance, counted on Dotty (Google has no API for it): turns and
// tokens since the last Pacific midnight, and "out of words until …" once Google refused.
namespace usage {

void addTurn(int tokens);
int turnsToday();
int tokensToday();
void markOut(int64_t untilUtc);
int64_t outUntilUtc();  // 0 when not out

}  // namespace usage
