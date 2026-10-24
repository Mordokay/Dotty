#pragma once

#include <stdint.h>

#include <string>
#include <vector>

// Scheduled moments (spec: docs/superpowers/specs/2026-10-24-companion-moments-design.md): what a
// companion will say by itself at a given minute — a reminder, an alarm, or a line of its own — kept
// per companion as data/companions/<id>/moments.json, and the rules shared by every companion
// (data/moment_rules.json). Times are local epoch seconds (Dotty's clock keeps local time, so
// gmtime_r gives the local date and time). Plain C++ (host-tested). The timer lives apart
// (timer_beeps.h).

enum class MomentKind { Reminder, Alarm, Spontaneous };
enum class Repeat { Once, Daily, Weekdays, Days };
enum class MomentState { Upcoming, Done, Missed };  // ringing and snoozed are runtime only (Ringing)

struct Moment {
  int id = 0;
  MomentKind kind = MomentKind::Reminder;
  int64_t at = 0;  // the next time it's due
  Repeat repeat = Repeat::Once;
  uint8_t days = 0;  // Repeat::Days: bit 0 = Monday … bit 6 = Sunday
  std::string what;              // what it's about (≤ 200 characters)
  std::string style = "spoken";  // spoken, sung, whispered, excited, calm
  std::string source = "chat";   // chat, app, companion (later: calendar, slack…)
  std::string line;              // the words to say, once written
  bool ready = false;            // recorded
  MomentState state = MomentState::Upcoming;
  int64_t created = 0;
};

class MomentList {
 public:
  static constexpr int kMax = 50, kWhatMax = 200;
  bool load(const std::string &json);  // {"next": N, "moments": [...]}; false = broken (the list stays empty)
  std::string save() const;
  const std::vector<Moment> &all() const { return moments_; }
  const Moment *find(int id) const;
  Moment *find(int id);
  int add(Moment m);  // -1: full, empty what, at ≤ 0, or a bad style
  // By id: what, at, style, kind, repeat, days (and line/ready/state as given). A changed what or style
  // means a new line: ready goes false.
  bool update(const Moment &m);
  bool remove(int id);

 private:
  std::vector<Moment> moments_;
  int next_ = 1;
};

// minutes since midnight; to < from = the window runs past midnight (it belongs to the day it starts)
struct QuietWindow {
  uint8_t days = 0x7F;  // bit 0 = Monday … bit 6 = Sunday
  int from = 0, to = 0;
};

struct MomentRules {
  int perDay = 2;              // spontaneous lines a day, 0-5
  int from = 600, to = 1260;   // the hours they may come in (minutes since midnight)
  std::vector<QuietWindow> quiet;
  bool quietSilent = false;    // a reminder in a quiet window: text only (else beeps and text)
  int alarmVolume = 90;        // 80-100 %
  int wakeEvery = 20;          // the phone wake's interval, minutes (10-240)
  bool load(const std::string &json);  // missing keys keep their defaults; values clamped
  std::string save() const;
  bool inQuiet(int64_t local) const;
  bool inAllowedHours(int64_t local) const;
};

bool parseLocalTime(const std::string &iso, int64_t &local);  // "YYYY-MM-DDTHH:MM" (":SS" optional)
std::string localTimeText(int64_t local);                     // "2026-10-24T18:00"
// The first time after `after` it's due: Once → m.at if it's later, else 0; repeating → the next allowed
// day at m.at's time of day, never before m.at.
int64_t nextOccurrence(const Moment &m, int64_t after);
// A new or moved repeating moment's first time on an allowed day ("weekdays 07:00" set for a Saturday →
// Monday). False when no day is allowed (Repeat::Days with none). A one-off is left as it is.
bool firstOccurrence(Moment &m);
const char *kindName(MomentKind k);  // "reminder", "alarm", "spontaneous"
bool kindFromName(const std::string &name, MomentKind &out);
const char *repeatName(Repeat r);  // "once", "daily", "weekdays", "days"
bool repeatFromName(const std::string &name, Repeat &out);
bool validStyle(const std::string &style);
int weekdayBit(int64_t local);  // bit for that day: Monday = 1 << 0 … Sunday = 1 << 6
