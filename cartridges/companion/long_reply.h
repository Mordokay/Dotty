#pragma once

#include <stdint.h>

#include <string>

// A reply longer than Gemini Live speaks in one go (measured: "one whole minute" gave 25-30 s,
// however it was asked). The model calls long_reply {seconds} before a long answer and speaks the
// first part without wrapping up; when a part has arrived, Dotty asks it to carry on where it
// stopped until the audio adds up to the length. Only the last part may conclude, so it sounds like
// one unbroken speech. Plain C++ (host-tested).
class LongReply {
 public:
  static constexpr uint32_t kMinMs = 30000, kMaxMs = 60000;  // Dotty keeps a minute of audio
  static constexpr int kMaxParts = 4;
  static constexpr uint32_t kCloseEnoughMs = 5000;
  // What's left is a short wrap-up: that part concludes. (With 32 s left a "last part" came out at
  // 11 s and ended; then more parts followed and ended again.)
  static constexpr uint32_t kLastPartMs = 20000;
  static constexpr uint32_t kMinLastS = 10;

  void begin(int seconds) {
    concluded_ = false;
    const uint32_t ms = seconds > 0 ? static_cast<uint32_t>(seconds) * 1000 : 0;
    target_ = ms < kMinMs ? kMinMs : ms > kMaxMs ? kMaxMs : ms;
    parts_ = 1;
  }
  void end() { target_ = 0, parts_ = 0, concluded_ = false; }
  bool active() const { return target_ > 0; }
  uint32_t targetMs() const { return target_; }
  int parts() const { return parts_; }

  // Another part after this one? The second always comes (the first was told not to wrap up).
  bool more(uint32_t receivedMs) const {
    return active() && !concluded_ && parts_ < kMaxParts && (parts_ == 1 || receivedMs + kCloseEnoughMs < target_);
  }

  // The note that asks for the next part.
  std::string next(uint32_t receivedMs) {
    parts_++;
    const uint32_t left = receivedMs < target_ ? target_ - receivedMs : 0;
    const bool last = left <= kLastPartMs || parts_ >= kMaxParts;
    concluded_ = last;  // the one ending: nothing is asked after it
    // A part is about 30 s (what Live speaks in one go): asked for less, it made 6-16 s slivers.
    const uint32_t seconds = last ? (left / 1000 < kMinLastS ? kMinLastS : left / 1000) : 30;
    return "(Dotty, not the user: your reply continues.) Carry on exactly where you stopped, as one unbroken speech: "
           "no greeting, no recap, don't repeat what you said, and don't mention this note. Speak about " +
           std::to_string(seconds) + " seconds in this part. " +
           (last ? "This is the last part: bring it to a natural end."
                 : "Don't wrap up yet: stop at the end of a sentence, without any closing words; more will follow.");
  }

 private:
  uint32_t target_ = 0;
  int parts_ = 0;
  bool concluded_ = false;
};
