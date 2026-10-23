// uses: cartridges/companion/last_line.cpp
// The last thing a companion said, kept for replay: its text pieces timed to the audio, a WAV
// header for the recording, and the play / pause / loop rules.
#include <cstring>

#include "check.h"
#include "last_line.h"

int main() {
  LastLine l;
  l.add(0, "Hello there. ");
  l.add(1200, "How was the ");
  l.add(2500, "surf today?");
  l.ms = 4000;
  l.mood = "happy";
  l.t = 1792800000;
  CHECK_EQ("text: every piece", l.text(), std::string("Hello there. How was the surf today?"));
  CHECK_EQ("text at 0 ms: the first piece", l.textAt(0), std::string("Hello there. "));
  CHECK_EQ("text at 2 s: two pieces", l.textAt(2000), std::string("Hello there. How was the "));
  CHECK_EQ("text at the end: all", l.textAt(9000), l.text());

  CHECK_EQ("progress: start", l.progress(0), 0);
  CHECK_EQ("progress: half way (per mille)", l.progress(2000), 500);
  CHECK_EQ("progress: past the end stays full", l.progress(9000), 1000);
  CHECK_EQ("progress: no length, nothing", LastLine().progress(10), 0);

  LastLine back;
  CHECK("json: round trip", parseLastLine(lastLineJson(l), back) && back.text() == l.text() && back.ms == 4000 &&
                                back.mood == "happy" && back.t == l.t && back.pieces.size() == 3 &&
                                back.pieces[1].ms == 1200);
  CHECK("json: broken refused", !parseLastLine("{oops", back));

  // A reply longer than the recording keeps: the text stops where the audio stops.
  LastLine longer = l;
  longer.cut(2000);
  CHECK("cut: pieces after the end dropped", longer.pieces.size() == 2 && longer.ms == 2000);
  LastLine shorter = l;
  shorter.cut(9000);
  CHECK("cut: nothing to cut", shorter.pieces.size() == 3 && shorter.ms == 4000);

  uint8_t h[44];
  wavHeader(h, 48000, 24000);
  CHECK("wav: RIFF/WAVE/fmt/data", !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4));
  auto u32 = [&](int at) { return uint32_t(h[at]) | uint32_t(h[at + 1]) << 8 | uint32_t(h[at + 2]) << 16 | uint32_t(h[at + 3]) << 24; };
  CHECK_EQ("wav: sizes", u32(4), uint32_t(36 + 48000));
  CHECK_EQ("wav: data size", u32(40), uint32_t(48000));
  CHECK_EQ("wav: rate", u32(24), uint32_t(24000));
  CHECK_EQ("wav: bytes per second (16-bit mono)", u32(28), uint32_t(48000));
  CHECK("wav: mono 16-bit PCM", h[20] == 1 && h[22] == 1 && h[32] == 2 && h[34] == 16);

  // Play / pause / loop.
  Replay r;
  CHECK("replay: stopped at first", r.state() == Replay::Stopped && !r.looping());
  r.toggle(0);
  CHECK("replay: toggle plays", r.state() == Replay::Playing);
  r.toggle(1000);
  CHECK("replay: toggle again pauses", r.state() == Replay::Paused);
  r.toggle(2000);
  CHECK("replay: and resumes", r.state() == Replay::Playing);
  CHECK("replay: the end without loop stops", r.ended(3000) == Replay::Stop && r.state() == Replay::Stopped);
  r.toggle(4000);
  r.setLoop(true, 4000);
  CHECK("replay: the end with loop starts over", r.ended(5000) == Replay::Again && r.state() == Replay::Playing);
  CHECK("replay: looping keeps going for 29 min untouched", !r.tooLong(4000 + 29u * 60 * 1000));
  CHECK("replay: but stops after 30", r.tooLong(4000 + 30u * 60 * 1000));
  r.touch(4000 + 29u * 60 * 1000);
  CHECK("replay: a touch starts the 30 minutes over", !r.tooLong(4000 + 31u * 60 * 1000));
  r.stop();
  CHECK("replay: stop keeps the loop setting", r.state() == Replay::Stopped && r.looping());
  CHECK("replay: not playing, never too long", !r.tooLong(4000 + 600u * 60 * 1000));
  return failures ? 1 : 0;
}
