#pragma once

#include <stdint.h>

#include <string>
#include <vector>

// The last thing the companion said, kept per companion for replay (last.wav + last.json in its
// folder): the reply's text in the pieces it arrived in, each timed to the audio that came with it,
// so a replay pages the text along with the voice as the first time. Plain C++ (host-tested).
struct LastLine {
  struct Piece {
    uint32_t ms;  // the audio received before this text arrived
    std::string text;
  };
  std::vector<Piece> pieces;
  uint32_t ms = 0;  // the recording's length
  std::string mood;
  int64_t t = 0;  // when it was said (local epoch)

  void add(uint32_t atMs, const std::string &text) { pieces.push_back({atMs, text}); }
  std::string text() const;
  std::string textAt(uint32_t positionMs) const;  // what has been heard by then
  void cut(uint32_t endMs);
  int progress(uint32_t positionMs) const {  // 0..1000 of the recording played
    return ms ? static_cast<int>((positionMs >= ms ? ms : positionMs) * 1000ull / ms) : 0;
  }  // the recording stopped there: later text goes too
};

std::string lastLineJson(const LastLine &l);
bool parseLastLine(const std::string &json, LastLine &out);

// A 44-byte WAV header for 16-bit mono PCM.
void wavHeader(uint8_t out[44], uint32_t dataBytes, uint32_t rate);

// Play / pause / loop for the replay. Looping stops after 30 minutes untouched (like Facts' player).
class Replay {
 public:
  enum State { Stopped, Playing, Paused };
  enum Next { Stop, Again };
  static constexpr uint32_t kLoopMaxMs = 30u * 60 * 1000;

  State state() const { return state_; }
  bool looping() const { return loop_; }
  void toggle(uint32_t now) {  // stopped / paused → playing, playing → paused
    state_ = state_ == Playing ? Paused : Playing;
    touchedAt_ = now;
  }
  void play(uint32_t now) {
    state_ = Playing;
    touchedAt_ = now;
  }
  void stop() { state_ = Stopped; }
  void setLoop(bool on, uint32_t now) {
    loop_ = on;
    touchedAt_ = now;
  }
  void touch(uint32_t now) { touchedAt_ = now; }
  Next ended(uint32_t now) {  // the recording reached its end
    (void)now;
    if (loop_ && state_ == Playing) return Again;
    state_ = Stopped;
    return Stop;
  }
  bool tooLong(uint32_t now) const { return state_ == Playing && loop_ && now - touchedAt_ >= kLoopMaxMs; }

 private:
  State state_ = Stopped;
  bool loop_ = false;
  uint32_t touchedAt_ = 0;
};
