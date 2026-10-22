// uses: cartridges/companion/usage_rules.cpp
// When Google's free allowance resets, the history file format, the day files, key clean-up.
#include <time.h>

#include "check.h"
#include "usage_rules.h"

static int64_t utc(int year, int month, int day, int hour, int minute = 0) {
  tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  return timegm(&t);
}

int main() {
  // Summer time (PDT, UTC-7): midnight Pacific = 07:00 UTC.
  CHECK_EQ("reset: October, summer time", nextPacificMidnightUtc(utc(2026, 10, 22, 12)), utc(2026, 10, 23, 7));
  // Winter time (PST, UTC-8): 08:00 UTC. DST ended on Sunday 2026-11-01.
  CHECK_EQ("reset: November, winter time", nextPacificMidnightUtc(utc(2026, 11, 5, 12)), utc(2026, 11, 6, 8));
  CHECK_EQ("reset: the evening before the change", nextPacificMidnightUtc(utc(2026, 11, 1, 5)), utc(2026, 11, 1, 7));
  CHECK_EQ("reset: the day of the change", nextPacificMidnightUtc(utc(2026, 11, 1, 12)), utc(2026, 11, 2, 8));
  // DST started on Sunday 2026-03-08.
  CHECK_EQ("reset: March, just after DST began", nextPacificMidnightUtc(utc(2026, 3, 10, 12)), utc(2026, 3, 11, 7));
  CHECK_EQ("reset: March, before DST", nextPacificMidnightUtc(utc(2026, 3, 5, 12)), utc(2026, 3, 6, 8));
  CHECK_EQ("reset: exactly at midnight counts as the next one", nextPacificMidnightUtc(utc(2026, 10, 23, 7)), utc(2026, 10, 24, 7));

  CHECK_EQ("history: one JSON line",
           historyLine(1790000000, "Tell me a \"joke\"", "Why?\nBecause.", "happy", 950, 412),
           std::string(R"({"t":1790000000,"you":"Tell me a \"joke\"","dotty":"Why?\nBecause.","mood":"happy","ms":950,"tokens":412})"));
  CHECK_EQ("history: a memory change at the end",
           historyLine(1790000000, "My sister is Ana", "Nice!", "happy", 900, 400, "remembered: Your sister is called Ana"),
           std::string(R"({"t":1790000000,"you":"My sister is Ana","dotty":"Nice!","mood":"happy","ms":900,"tokens":400,)"
                       R"("memory":"remembered: Your sister is called Ana"})"));
  CHECK_EQ("history: the day's file", dayFile(utc(2026, 10, 22, 23, 59)), std::string("2026-10-22.jsonl"));

  CHECK_EQ("key: spaces and a newline trimmed", trimKey("  AIza123 \n"), std::string("AIza123"));
  CHECK_EQ("key: only whitespace is no key", trimKey("\n"), std::string(""));
  CHECK_EQ("key: inner text kept", trimKey("\tAQ.Xy7Zq2W\r\n"), std::string("AQ.Xy7Zq2W"));
  return failures ? 1 : 0;
}
