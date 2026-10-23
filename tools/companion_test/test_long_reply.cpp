// uses:
// A reply longer than Gemini Live speaks in one go (~25-30 s): Dotty asks it to carry on, part
// after part, until the length the user asked for; only the last part may conclude.
#include "check.h"
#include "long_reply.h"

static bool has(const std::string &s, const char *part) {
  return s.find(part) != std::string::npos;
}

int main() {
  LongReply r;
  CHECK("idle: no more parts", !r.active() && !r.more(0));
  r.begin(60);
  CHECK("begun", r.active() && r.targetMs() == 60000);
  CHECK("a second part always follows the first (it was told not to wrap up)", r.more(58000));
  const std::string second = r.next(28000);
  CHECK("prompt: carry on where it stopped", has(second, "Carry on exactly where you stopped"));
  CHECK("prompt: no recap or greeting", has(second, "no greeting") && has(second, "don't repeat"));
  // 32 s left is more than a last part should carry (asked for 32, a last part gave 11 and ended).
  CHECK("32 s left: a full middle part that doesn't conclude", has(second, "Don't wrap up yet") && has(second, "Speak about 30 seconds"));
  CHECK("done once it's long enough", !r.more(56000));
  CHECK("not done while far short", r.more(45000));
  const std::string third = r.next(45000);
  CHECK("15 s left: the last part concludes", has(third, "last part") && has(third, "Speak about 15 seconds"));
  CHECK("after the last part, nothing more (even if it came out short)", !r.more(47000));

  LongReply tiny;
  tiny.begin(60);
  tiny.next(20000);
  CHECK("a last part is at least 10 s", has(tiny.next(58000), "Speak about 10 seconds"));

  LongReply capped;
  capped.begin(600);
  CHECK("capped at a minute (Dotty keeps a minute of audio)", capped.targetMs() == 60000);
  capped.begin(5);
  CHECK("at least 30 s (it was asked for something long)", capped.targetMs() == 30000);

  LongReply parts;
  parts.begin(60);
  for (int i = 0; i < 3; i++) parts.next(1000);
  CHECK("at most 4 parts", !parts.more(1000));
  parts.end();
  CHECK("end: idle again", !parts.active() && !parts.more(0));
  return failures ? 1 : 0;
}
