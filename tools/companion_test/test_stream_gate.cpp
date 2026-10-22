// uses:
// When streamed reply audio plays (lib/dotty_core/src/stream_gate.h): a quarter second in hand to
// start, a whole second after running dry, so a slow network gives a few pauses, not a stutter.
#include "check.h"
#include "stream_gate.h"

int main() {
  StreamGate g(24000);
  CHECK("start: waits for a quarter second", !g.canPlay(5999, false) && g.canPlay(6000, false));
  CHECK("playing: plays what there is", g.canPlay(1, false));
  g.ranDry();
  CHECK("after running dry: waits for a second", !g.canPlay(23999, false) && g.canPlay(24000, false));
  CHECK("then plays on", g.canPlay(10, false));
  g.ranDry();
  CHECK("the end of a reply: plays what's left", g.canPlay(100, true));
  CHECK_EQ("counts the pauses", g.pauses(), 2);
  return failures ? 1 : 0;
}
