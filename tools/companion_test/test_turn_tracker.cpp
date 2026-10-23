// uses:
// Which of Gemini's messages belong to the question you asked (cartridges/companion/turn_tracker.h).
#include "check.h"
#include "turn_tracker.h"

using Names = std::vector<std::string>;

int main() {
  TurnTracker t;
  t.start();
  TurnTracker::Out o = t.message(false, Names{"set_mood"}, false, 0);  // set_mood first
  CHECK("mood: applied and answered once", o.apply.at(0) && o.answer.at(0));
  o = t.message(false, Names{"set_mood"}, false, 0);
  CHECK("mood: a second call isn't answered", o.apply.at(0) && !o.answer.at(0));
  o = t.message(false, Names{"remember", "forget_memory"}, false, 0);
  CHECK("memory calls in a question: applied", o.apply.at(0) && o.apply.at(1));
  CHECK("memory calls in a question: answered", o.answer.at(0) && o.answer.at(1));
  o = t.message(false, Names{"recall"}, false, 0);
  CHECK("recall in a question: applied, not answered (Dotty answers with what it found)", o.apply.at(0) && !o.answer.at(0));
  o = t.message(false, Names{"set_name"}, false, 0);
  CHECK("set_name in a question: applied and answered", o.apply.at(0) && o.answer.at(0));
  o = t.message(false, Names{}, true, 800);  // its own turn, no speech yet
  CHECK("mood turn: not the reply", !o.done && !o.close);
  o = t.message(true, Names{}, false, 0);
  CHECK("speech: delivered", o.deliver);
  o = t.message(false, Names{}, true, 900);
  CHECK_EQ("reply done with every token of the question", o.done ? o.tokens : -1, 1700);

  // Interrupted: the old reply's leftovers must not land in the new question.
  TurnTracker i;
  i.start();
  i.message(true, Names{}, false, 0);  // the first reply is speaking...
  i.start();                           // ...and you press BOOT again
  o = i.message(true, Names{"remember"}, false, 0);
  CHECK("interrupted: old audio dropped", !o.deliver);
  CHECK("interrupted: its memory call not applied", !o.apply.at(0) && !o.answer.at(0));
  o = i.message(false, Names{}, true, 500);
  CHECK("interrupted: its end isn't the new reply", !o.done && !o.close);
  CHECK_EQ("interrupted: its tokens still count as spent", o.spent, 500);
  o = i.message(false, Names{"set_mood"}, false, 0);
  CHECK("new question: its mood is answered", o.apply.at(0) && o.answer.at(0));
  o = i.message(true, Names{}, false, 0);
  CHECK("new question: its speech is delivered", o.deliver);
  o = i.message(false, Names{}, true, 700);
  CHECK_EQ("new question: done with its own tokens", o.done ? o.tokens : -1, 700);

  // Nobody asked: nothing applied, tokens counted, three such turns close the session.
  TurnTracker r;
  o = r.message(true, Names{}, false, 0);
  CHECK("unasked: speech not delivered", !o.deliver);
  o = r.message(false, Names{"set_mood", "remember"}, false, 0);
  CHECK("unasked: no tool applied or answered", !o.apply.at(0) && !o.apply.at(1) && !o.answer.at(0) && !o.answer.at(1));
  o = r.message(false, Names{"set_name"}, false, 0);
  CHECK("unasked: set_name not applied", !o.apply.at(0));
  o = r.message(false, Names{}, true, 300);
  CHECK("unasked: 1st turn tolerated, tokens spent", !o.close && o.spent == 300);
  r.message(false, Names{}, true, 300);
  o = r.message(false, Names{}, true, 300);
  CHECK("unasked: 3rd turn closes", o.close);
  return failures ? 1 : 0;
}
