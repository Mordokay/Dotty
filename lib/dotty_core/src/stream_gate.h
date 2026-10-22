#pragma once

#include <stddef.h>

// When streamed audio (a reply arriving over the network) may play: a quarter second in hand to
// start, a whole second after the buffer ran dry. On a weak Wi-Fi (-78 dBm, 2026-10-22) playing
// whatever had arrived gave 1,137 tiny gaps in one reply; waiting to refill gives a few pauses.
class StreamGate {
 public:
  explicit StreamGate(size_t sampleRate) : rate_(sampleRate) {}

  // `available` frames buffered; `ending` = no more will come (play out what's left).
  bool canPlay(size_t available, bool ending) {
    if (!waiting_) return true;
    if (!ending && available < (pauses_ ? rate_ : rate_ / 4)) return false;
    waiting_ = false;
    return true;
  }
  void ranDry() {
    waiting_ = true;
    pauses_++;
  }
  int pauses() const { return pauses_; }

 private:
  size_t rate_;
  bool waiting_ = true;
  int pauses_ = 0;
};
