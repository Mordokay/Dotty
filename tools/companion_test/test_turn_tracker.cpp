// uses:
// Which of Gemini's messages belong to the question you asked (cartridges/companion/turn_tracker.h).
#include "check.h"
#include "turn_tracker.h"

int main() {
  TurnTracker t;
  t.start();
  TurnTracker::Out o = t.message(false, true, false, 0);  // set_mood first
  CHECK("mood: answered once, shown", o.answerTool && o.showMood);
  o = t.message(false, true, false, 0);
  CHECK("mood: a second call isn't answered", !o.answerTool);
  o = t.message(false, false, true, 800);  // its own turn, no speech yet
  CHECK("mood turn: not the reply", !o.done && !o.close);
  o = t.message(true, false, false, 0);
  CHECK("speech: delivered", o.deliver);
  o = t.message(false, false, true, 900);
  CHECK_EQ("reply done with every token of the question", o.done ? o.tokens : -1, 1700);

  // Interrupted: the old reply's leftovers must not land in the new question.
  TurnTracker i;
  i.start();
  i.message(true, false, false, 0);  // the first reply is speaking...
  i.start();                         // ...and you press BOOT again
  o = i.message(true, false, false, 0);
  CHECK("interrupted: old audio dropped", !o.deliver);
  o = i.message(false, false, true, 500);
  CHECK("interrupted: its end isn't the new reply", !o.done && !o.close);
  CHECK_EQ("interrupted: its tokens still count as spent", o.spent, 500);
  o = i.message(false, true, false, 0);
  CHECK("new question: its mood is answered", o.answerTool && o.showMood);
  o = i.message(true, false, false, 0);
  CHECK("new question: its speech is delivered", o.deliver);
  o = i.message(false, false, true, 700);
  CHECK_EQ("new question: done with its own tokens", o.done ? o.tokens : -1, 700);

  // Nobody asked: tokens counted, three such turns close the session.
  TurnTracker r;
  o = r.message(true, false, false, 0);
  CHECK("unasked: speech not delivered", !o.deliver);
  o = r.message(false, true, false, 0);
  CHECK("unasked: tool not answered", !o.answerTool && !o.showMood);
  o = r.message(false, false, true, 300);
  CHECK("unasked: 1st turn tolerated, tokens spent", !o.close && o.spent == 300);
  r.message(false, false, true, 300);
  o = r.message(false, false, true, 300);
  CHECK("unasked: 3rd turn closes", o.close);
  return failures ? 1 : 0;
}
