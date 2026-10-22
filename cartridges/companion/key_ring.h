#pragma once

#include <stdint.h>

#include <vector>

// Up to 3 Gemini API keys taken in turn, one per session (the user's design: a conversation stays
// on one key, the next session uses the next); a key out of its free allowance is skipped until its
// reset. Plain C++, host-tested; main.cpp keeps `next` and the out times in NVS.
class KeyRing {
 public:
  static constexpr int kMax = 3;

  void setCount(int n) {
    out_.resize(n < 0 ? 0 : n > kMax ? kMax : n, 0);
    if (next_ >= static_cast<int>(out_.size())) next_ = 0;
  }
  int count() const { return out_.size(); }
  int next() const { return next_; }
  int64_t outUntil(int i) const { return i >= 0 && i < count() ? out_[i] : 0; }
  void restore(int next, const std::vector<int64_t> &outUntil) {
    for (int i = 0; i < count() && i < static_cast<int>(outUntil.size()); i++) out_[i] = outUntil[i];
    next_ = next >= 0 && next < count() ? next : 0;
  }

  // The key for a new session (-1: none set, or all out), and the one after it for the next.
  int pick(int64_t now) {
    for (int k = 0; k < count(); k++) {
      const int i = (next_ + k) % count();
      if (out_[i] > now) continue;
      next_ = (i + 1) % count();
      return i;
    }
    return -1;
  }
  void markOut(int i, int64_t until) {
    if (i >= 0 && i < count()) out_[i] = until;
  }
  // When every key is out: the first time one comes back; else 0.
  int64_t allOutUntil(int64_t now) const {
    int64_t first = 0;
    for (int64_t t : out_) {
      if (t <= now) return 0;
      if (!first || t < first) first = t;
    }
    return first;
  }

 private:
  std::vector<int64_t> out_;
  int next_ = 0;
};
