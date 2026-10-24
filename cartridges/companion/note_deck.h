#pragma once

#include <stdint.h>

#include <vector>

// The recording deck's rules (spec: docs/superpowers/specs/2026-10-24-companion-recordings-design.md,
// "Recording on Dotty"): BOOT held = record a part, released = pause; Undo drops the last part; Done keeps
// it; Discard and the back arrow ask first when something was recorded (BOOT on that question records
// again); locking = Done. Limits: nothing recorded within 2 minutes closes it, 5 minutes paused is Done,
// a warning at 29:30 of recording and Done at 30:00. Plain C++ (host-tested); main.cpp acts on the events.
class NoteDeck {
 public:
  static constexpr uint32_t kIdleCloseMs = 120000, kPausedDoneMs = 300000, kMaxMs = 1800000, kWarnMs = 1770000;
  static constexpr uint32_t kConfirmMs = 10000;  // the discard question goes away (as Tape's)
  static constexpr uint32_t kMinPartMs = 200;    // shorter holds are a bounce, not a part
  enum class State { Closed, Waiting, Recording, Paused, ConfirmDiscard };
  enum class Event { None, PartStart, PartEnd, PartDropped, Undo, Warn, Done, Discard, CloseEmpty };

  void open(uint32_t now) {
    *this = NoteDeck();
    state_ = State::Waiting;
    openedAt_ = now;
  }
  Event hold(bool down, uint32_t now) {
    if (down && (state_ == State::Waiting || state_ == State::Paused || state_ == State::ConfirmDiscard)) {
      state_ = State::Recording;
      partStart_ = now;
      return Event::PartStart;
    }
    if (!down && state_ == State::Recording) {
      if (now - partStart_ < kMinPartMs) {  // a bounce: main.cpp cuts it off the card (note::undo)
        state_ = parts_.empty() ? State::Waiting : State::Paused;
        pausedAt_ = now;
        return Event::PartDropped;
      }
      endPart(now);
      return Event::PartEnd;
    }
    return Event::None;
  }
  Event undo() {
    if (state_ != State::Paused || parts_.empty()) return Event::None;
    parts_.pop_back();
    return Event::Undo;
  }
  Event done() { return finish(); }
  Event discard(bool confirmed, uint32_t now = 0) {
    if (parts_.empty() && state_ != State::Recording) return close(Event::CloseEmpty);
    if (!confirmed) {
      state_ = State::ConfirmDiscard;
      askedAt_ = now ? now : pausedAt_;
      return Event::None;
    }
    return close(Event::Discard);
  }
  void keep() {  // "No" to the discard question
    if (state_ == State::ConfirmDiscard) state_ = parts_.empty() ? State::Waiting : State::Paused;
  }
  Event cancel(uint32_t now = 0) { return discard(false, now); }
  Event lockOrPower(uint32_t now) {
    if (state_ == State::Recording) endPart(now);
    return finish();
  }
  Event tick(uint32_t now) {
    switch (state_) {
      case State::Waiting:
        return now - openedAt_ >= kIdleCloseMs ? close(Event::CloseEmpty) : Event::None;
      case State::Paused:
        return now - pausedAt_ >= kPausedDoneMs ? finish() : Event::None;
      case State::ConfirmDiscard:
        if (now - askedAt_ >= kConfirmMs) {  // unanswered: back to paused, its 5 minutes start again
          state_ = parts_.empty() ? State::Waiting : State::Paused;
          pausedAt_ = openedAt_ = now;
        }
        return Event::None;
      case State::Recording: {
        const uint32_t total = recordedMs() + (now - partStart_);
        if (total >= kMaxMs) {
          endPart(now);
          return finish();
        }
        if (total >= kWarnMs && !warned_) {
          warned_ = true;
          return Event::Warn;
        }
        return Event::None;
      }
      default:
        return Event::None;
    }
  }
  State state() const { return state_; }
  uint32_t elapsedMs(uint32_t now) const { return recordedMs() + (state_ == State::Recording ? now - partStart_ : 0); }
  int parts() const { return static_cast<int>(parts_.size()); }
  uint32_t lastPartMs() const { return parts_.empty() ? 0 : parts_.back(); }

 private:
  void endPart(uint32_t now) {
    parts_.push_back(now - partStart_);
    pausedAt_ = now;
    state_ = State::Paused;
  }
  Event finish() {
    return close(parts_.empty() ? Event::CloseEmpty : Event::Done);
  }
  Event close(Event e) {
    state_ = State::Closed;
    return e;
  }
  uint32_t recordedMs() const {
    uint32_t total = 0;
    for (const uint32_t p : parts_) total += p;
    return total;
  }
  State state_ = State::Closed;
  std::vector<uint32_t> parts_;  // each part's length
  uint32_t openedAt_ = 0, partStart_ = 0, pausedAt_ = 0, askedAt_ = 0;
  bool warned_ = false;
};
