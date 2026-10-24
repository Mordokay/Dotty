#include "timer_beeps.h"

#include <cctype>
#include <cstdio>

bool TimerState::start(int seconds, const std::string &label_, int tickEvery_, const std::string &companion_, int64_t now) {
  if (seconds <= 0 || seconds > kMaxSeconds) return false;
  running = true;
  paused = false;
  endsAt = now + seconds;
  left = 0;
  tickEvery = tickEvery_ > 0 ? tickEvery_ : 0;
  label = label_;
  companion = companion_;
  line.clear();
  ready = false;
  return true;
}

void TimerState::pause(int64_t now) {
  if (!running || paused) return;
  left = secondsLeft(now);
  paused = true;
}

void TimerState::resume(int64_t now) {
  if (!running || !paused) return;
  endsAt = now + left;
  paused = false;
}

void TimerState::cancel() {
  *this = TimerState();
}

int TimerState::secondsLeft(int64_t now) const {
  if (!running) return 0;
  if (paused) return left;
  return endsAt > now ? static_cast<int>(endsAt - now) : 0;
}

std::string TimerState::title() const {
  if (label.empty()) return "Timer";
  std::string t = label;
  t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
  return t;
}

std::string TimerState::lockLine(int64_t now) const {
  const int minutes = (secondsLeft(now) + 59) / 60;
  return title() + " - " + std::to_string(minutes) + " min";
}

std::string TimerState::clock(int64_t now) const {
  const int s = secondsLeft(now);
  char buf[16];
  if (s >= 3600) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
  else std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
  return buf;
}

TimerCue timerCue(int secondsLeft, int tickEvery, bool busy) {
  TimerCue cue;
  if (secondsLeft <= 0) {
    cue.sound = TimerSound::End;
    return cue;
  }
  if (busy) return cue;
  if (secondsLeft <= 10) {
    cue.sound = TimerSound::Countdown;
    cue.beep.midi = static_cast<uint8_t>(72 + (10 - secondsLeft));
    cue.beep.ms = 120;
    cue.beep.volume = static_cast<uint8_t>(50 + (10 - secondsLeft) * 50 / 9);
    return cue;
  }
  const bool interval = tickEvery > 0 && secondsLeft > 60 && secondsLeft % tickEvery == 0;
  if (secondsLeft == 60 || secondsLeft == 30 || interval) {
    cue.sound = TimerSound::Tick;
    cue.beep.midi = 84;
    cue.beep.ms = 30;
    cue.beep.volume = 40;
  }
  return cue;
}

int timerRefreshSeconds(int secondsLeft) {
  return secondsLeft <= 60 ? 1 : 10;
}
