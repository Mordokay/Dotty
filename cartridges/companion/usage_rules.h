#pragma once

#include <stdint.h>

#include <string>

// Rules for usage counting and history, with no I/O (tested on a computer, tools/companion_test).

// Google's free Gemini allowance resets at midnight US Pacific time: the next one after `nowUtc`
// (UTC epoch seconds), with US daylight saving (second Sunday of March to first Sunday of
// November, switching at 02:00 local).
int64_t nextPacificMidnightUtc(int64_t nowUtc);

// One line of data/history/<day>.jsonl (no trailing newline).
std::string historyLine(int64_t localEpoch, const std::string &you, const std::string &dotty, const std::string &mood,
                        int ms, int tokens,
                        const std::string &memory = "");
// The day's history file name for a local time: "2026-10-22.jsonl".
std::string dayFile(int64_t localEpoch);
// Talks in a day's history (its .jsonl text): turns less than `gapSeconds` apart are one talk.
int conversations(const std::string &dayLog, int gapSeconds);
// An API key as pasted: surrounding spaces, tabs and newlines removed.
std::string trimKey(const std::string &raw);
