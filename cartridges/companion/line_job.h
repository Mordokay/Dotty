#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "moment_lines.h"
#include "script_writer.h"

// Recording one moment's line (spec: "Making the lines"), step by step: the text model writes it, a Live
// session reads it word for word in chunks of ~100 words (the next chunk as soon as the last has
// arrived: nothing plays, so no pacing), the audio is saved. Anything that interrupts it (BOOT, a lock,
// the phone leaving, quota) ends it Failed, and the moment stays not ready. Plain C++ (host-tested);
// main.cpp feeds it.
// How long a reading may take before it counts as stuck: Live streams it at about real time (~0.4 s a
// word) plus a pause at each chunk's join; half a second a word and 30 s spare (300 words: 3 minutes).
inline uint32_t readingCapMs(int words) {
  return static_cast<uint32_t>(words) * 500 + 30000;
}

struct LineWork {
  std::string companion;
  std::string key;  // the recording's name: "<id>", "group-<minute>" or "timer"
  std::vector<int> ids;
  std::vector<Topic> topics;
  int64_t at = 0;
  int words = 150;
  std::string style = "spoken";
  std::string signature;  // what it was made for (moment_schedule.h groupSignature)
};

class LineJob {
 public:
  enum class Step { Idle, Writing, WaitSession, Reading, Saving, Done, Failed };

  void start(const LineWork &w) {
    work_ = w;
    text_.clear();
    chunks_.clear();
    next_ = 0;
    pending_ = false;
    step_ = Step::Writing;
  }
  void written(const Written &w) {
    if (step_ != Step::Writing) return;
    if (!w.ok) {
      step_ = Step::Failed;
      return;
    }
    const size_t a = w.text.find_first_not_of(" \t\r\n"), b = w.text.find_last_not_of(" \t\r\n");
    text_ = a == std::string::npos ? std::string() : w.text.substr(a, b - a + 1);
    chunks_ = readingChunks(text_, 100);
    if (chunks_.empty() && !text_.empty()) chunks_.push_back(text_);
    step_ = chunks_.empty() ? Step::Failed : Step::WaitSession;
  }
  void sessionReady() {
    if (step_ != Step::WaitSession) return;
    step_ = Step::Reading;
    pending_ = true;
  }
  // The next text to send to Live (the first chunk with the reading instructions, then "carry on").
  bool takeSend(std::string &note) {
    if (step_ != Step::Reading || !pending_ || next_ >= chunks_.size()) return false;
    note = next_ == 0 ? readLineNote(chunks_[0], work_.style) : continueReadingNote(chunks_[next_]);
    pending_ = false;
    return true;
  }
  void chunkDone() {  // a reading turn ended
    if (step_ != Step::Reading || pending_) return;
    if (++next_ < chunks_.size()) pending_ = true;
    else step_ = Step::Saving;
  }
  void saved(bool ok) {
    if (step_ == Step::Saving) step_ = ok ? Step::Done : Step::Failed;
  }
  void abort() {
    if (step_ != Step::Idle && step_ != Step::Done) step_ = Step::Failed;
  }
  void reset() { step_ = Step::Idle; }
  Step step() const { return step_; }
  bool busy() const { return step_ != Step::Idle && step_ != Step::Done && step_ != Step::Failed; }
  const LineWork &work() const { return work_; }
  const std::string &text() const { return text_; }

 private:
  Step step_ = Step::Idle;
  LineWork work_;
  std::string text_;
  std::vector<std::string> chunks_;
  size_t next_ = 0;
  bool pending_ = false;
};
