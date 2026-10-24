#pragma once

#include <stdint.h>

#include <string>

// The timer (spec: "Timers"): one at a time, up to 24 h, kept in NVS so a restart keeps it. What it
// plays as it counts down: an optional tick every tickEvery seconds (not in the last minute), a tick at
// 60 and 30 s, then the last ten seconds a beep a second, each a semitone higher and louder (50 → 100 %),
// and the end (a fanfare and the companion's line, played by main.cpp). Nothing but the end while
// Dotty listens or speaks. Plain C++ (host-tested). Times are local epoch seconds.

struct TimerState {
  static constexpr int kMaxSeconds = 86400;
  bool running = false, paused = false;
  int64_t endsAt = 0;
  int left = 0;  // seconds, while paused
  int tickEvery = 0;
  std::string label, companion;
  std::string line;    // the companion's closing words, once written
  bool ready = false;  // its line is recorded

  bool start(int seconds, const std::string &label, int tickEvery, const std::string &companion, int64_t now);
  void pause(int64_t now);
  void resume(int64_t now);
  void cancel();
  int secondsLeft(int64_t now) const;       // never below 0
  std::string lockLine(int64_t now) const;  // "Pasta - 7 min" (minutes rounded up; "Timer" without a label)
  std::string clock(int64_t now) const;     // "9:41", "1:02:05"
  std::string title() const;                // "Pasta", or "Timer"
};

struct Beep {
  uint8_t midi = 0;
  uint16_t ms = 0;
  uint8_t volume = 0;  // %
};
enum class TimerSound { None, Tick, Countdown, End };
struct TimerCue {
  TimerSound sound = TimerSound::None;
  Beep beep;
};

TimerCue timerCue(int secondsLeft, int tickEvery, bool busy);  // asked once for each second reached
int timerRefreshSeconds(int secondsLeft);                      // 10, or 1 in the last minute
