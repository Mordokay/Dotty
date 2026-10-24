#pragma once

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "moment_store.h"

// When moments fire (spec: docs/superpowers/specs/2026-10-24-companion-moments-design.md, "How a moment
// fires" and "Several things at once"): a companion's moments due at the same minute are one group —
// one set of beeps, one line covering every topic. A moment less than an hour late still fires; an
// older one is missed. Alarms ring until answered (Ringing). Plain C++ (host-tested).

struct CompanionMoments {
  std::string companion;
  MomentList *list;
};

enum class Fate { Wait, Fire, Missed };
Fate fate(int64_t at, int64_t now);  // at > now: Wait; less than an hour late: Fire; else Missed

struct FireGroup {
  std::string companion;
  int64_t minute = 0;    // at / 60
  std::vector<int> ids;  // by id
  bool alarm = false;    // the strongest kind wins: an alarm in it rings
};

struct Due {
  std::vector<FireGroup> fire;                         // in companion order, then by minute
  std::vector<std::pair<std::string, int>> missed;     // companion, id
};

// Upcoming moments whose time has come. busy (listening, or a reply playing): nothing to fire yet;
// missed ones are still reported.
Due due(const std::vector<CompanionMoments> &all, int64_t now, bool busy);
// After it fired or was missed: a repeating moment moves to its next time (Upcoming, its recording
// kept); a one-off is Done or Missed.
void advance(Moment &m, int64_t now, bool missed);
int64_t nextDue(const std::vector<CompanionMoments> &all);  // the earliest Upcoming time, 0 = none

enum class Delivery { Voice, BeepsAndText, TextOnly };
Delivery delivery(const MomentRules &rules, bool alarm, int64_t now);  // alarms always Voice

int groupWords(size_t topics);  // 150 for one topic, + 100 each extra, at most 300
// What a group's recording was made for (ids, what, style; not the time): a change = record again.
std::string groupSignature(const std::vector<const Moment *> &members);
// Upcoming moments of one list sharing a minute with another (ids per minute, by time): they get one
// line together.
std::vector<std::vector<int>> sharedMinutes(const MomentList &list);

// An alarm (or a timer's end) ringing: it plays at once and every minute, gives up after 10 minutes
// unanswered; a snooze (alarms only, 3 at most) rings again 5 minutes later and starts the 10 over.
class Ringing {
 public:
  static constexpr uint32_t kRepeatMs = 60000, kMaxMs = 600000, kSnoozeMs = 300000;
  static constexpr int kMaxSnoozes = 3;
  enum class Action { None, Play, GiveUp };
  void start(uint32_t now, bool snoozable) {
    active_ = true;
    snoozable_ = snoozable;
    snoozed_ = false;
    snoozes_ = 0;
    startedAt_ = nextPlay_ = now;
  }
  Action poll(uint32_t now) {
    if (!active_) return Action::None;
    if (snoozed_) {
      if (static_cast<int32_t>(now - snoozedUntil_) < 0) return Action::None;
      snoozed_ = false;
      startedAt_ = nextPlay_ = now;
    }
    if (now - startedAt_ >= kMaxMs) {
      active_ = false;
      return Action::GiveUp;
    }
    if (static_cast<int32_t>(now - nextPlay_) >= 0) {
      nextPlay_ = now + kRepeatMs;
      return Action::Play;
    }
    return Action::None;
  }
  bool snooze(uint32_t now) {
    if (!active_) return false;
    if (!snoozable_ || snoozes_ >= kMaxSnoozes) {
      stop();
      return false;
    }
    snoozes_++;
    snoozed_ = true;
    snoozedUntil_ = now + kSnoozeMs;
    return true;
  }
  void stop() { active_ = snoozed_ = false; }
  bool active() const { return active_; }
  bool snoozed() const { return snoozed_; }
  int snoozes() const { return snoozes_; }

 private:
  bool active_ = false, snoozable_ = false, snoozed_ = false;
  int snoozes_ = 0;
  uint32_t startedAt_ = 0, nextPlay_ = 0, snoozedUntil_ = 0;
};
