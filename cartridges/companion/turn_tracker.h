#pragma once

#include <string>
#include <vector>

// Which of Gemini's messages belong to the question you asked (plain C++, host-tested).
//  - Function calls are applied only inside a question. set_mood is answered once per question
//    (each answer made Gemini speak again, a loop that ran by itself for minutes); memory calls
//    are each answered (see the companion-memories plan, Task 3). recall isn't: Dotty answers it
//    later with what it found (GeminiLive::answerTool, which opens a question for the reply).
//  - Asking again mid-reply (an interruption) drops the old reply's leftovers until its turn
//    ends, so they don't land in the new question (a phantom history entry, the real reply lost).
//  - Turns nobody asked for are counted as spent tokens; three of them close the session.
struct TurnTracker {
  struct Out {
    bool deliver = false;      // audio / text for the question being answered
    std::vector<bool> apply;   // per function call: act on it
    std::vector<bool> answer;  // per function call: send its response
    bool done = false;        // the reply ended: `tokens` is the question's total
    int tokens = 0;
    int spent = 0;            // tokens outside any question (interrupted or unasked turns)
    bool close = false;       // runaway: close the session
  };

  bool turn = false;  // a question is open (from start() until its reply ends)

  void start() {
    draining_ = turn;  // the last reply hasn't ended: its leftovers are still coming
    turn = true;
    speech_ = moodAnswered_ = false;
    tokens_ = unasked_ = 0;
  }

  // One message: did it carry audio/text, its function calls' names, the end of a turn, and its
  // token count.
  Out message(bool content, const std::vector<std::string> &toolNames, bool turnComplete, int tokens) {
    Out o;
    o.apply.assign(toolNames.size(), false);
    o.answer.assign(toolNames.size(), false);
    if (draining_) {
      o.spent = tokens;
      if (turnComplete) draining_ = false;
      return o;
    }
    for (size_t i = 0; i < toolNames.size() && turn; i++) {
      o.apply[i] = true;
      if (toolNames[i] == "recall") continue;  // Dotty answers it later, with what it found
      if (toolNames[i] != "set_mood") o.answer[i] = true;
      else if (!moodAnswered_) o.answer[i] = moodAnswered_ = true;
    }
    if (content && turn) {
      speech_ = true;
      o.deliver = true;
    }
    if (turn) tokens_ += tokens;
    else o.spent = tokens;
    if (turnComplete) {
      if (turn && speech_) {  // a turn with only set_mood isn't the reply: speech follows
        o.done = true;
        o.tokens = tokens_;
        turn = false;
      } else if (!turn && ++unasked_ >= 3) {
        o.close = true;
      }
    }
    return o;
  }

 private:
  bool draining_ = false, speech_ = false, moodAnswered_ = false;
  int tokens_ = 0, unasked_ = 0;
};
