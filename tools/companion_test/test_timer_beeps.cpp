// uses: cartridges/companion/timer_beeps.cpp
// The timer ("start a 10-minute timer"): its countdown, and what it plays as it goes — optional ticks,
// ticks at 60 and 30 s, the last ten seconds rising in pitch and volume, then the end.
#include "check.h"
#include "timer_beeps.h"

int main() {
  const int64_t t = 1793124000;
  {
    TimerState a, b, c;
    CHECK("start: 0 and 24 h + 1 refused; 10 minutes runs",
          !a.start(0, "", 0, "friend", t) && !b.start(86401, "", 0, "friend", t) && c.start(600, "pasta", 0, "friend", t) &&
              c.running && c.endsAt == t + 600);
  }
  {
    TimerState s;
    s.start(600, "pasta", 0, "friend", t);
    const int at100 = s.secondsLeft(t + 100);
    s.pause(t + 100);
    const int held = s.secondsLeft(t + 200);
    s.resume(t + 200);
    CHECK("secondsLeft counts down; pause holds; resume carries on",
          at100 == 500 && held == 500 && s.secondsLeft(t + 200) == 500 && s.endsAt == t + 700 && !s.paused &&
              s.secondsLeft(t + 9999) == 0);
  }
  {
    TimerState s, u;
    s.start(600, "pasta", 0, "friend", t);
    u.start(30, "", 0, "friend", t);
    CHECK("lockLine: Pasta - 7 min; Timer - 1 min at 30 s",
          s.lockLine(t + 181) == "Pasta - 7 min" && u.lockLine(t) == "Timer - 1 min");
    CHECK("clock: 9:41, 1:02:05, 0:00", s.clock(t + 19) == "9:41" && s.clock(t + 600) == "0:00");
    TimerState h;
    h.start(3725, "", 0, "friend", t);
    CHECK("clock: hours", h.clock(t) == "1:02:05");
  }
  {
    TimerState s;
    s.start(60, "eggs", 0, "friend", t);
    s.what = "sing happy birthday";
    s.style = "sung";
    s.line = "old words";
    s.ready = true;
    s.start(60, "", 0, "friend", t);
    CHECK("start forgets the last timer's words, style and line", s.what.empty() && s.style.empty() && s.line.empty() && !s.ready);
    s.what = "sing happy birthday";
    s.style = "sung";
    TimerState other = s;
    other.what = "say well done";
    CHECK("signature: what, style and label make the recording", s.signature() != other.signature() &&
                                                                     s.signature().find("happy birthday") != std::string::npos);
  }
  CHECK("cue: no tickEvery -> silence at 300 s", timerCue(300, 0, false).sound == TimerSound::None);
  CHECK("cue: tickEvery 60 -> a tick at 120 s, none at 90",
        timerCue(120, 60, false).sound == TimerSound::Tick && timerCue(90, 60, false).sound == TimerSound::None);
  CHECK("cue: ticks at 60 and 30 even without tickEvery",
        timerCue(60, 0, false).sound == TimerSound::Tick && timerCue(30, 0, false).sound == TimerSound::Tick &&
            timerCue(45, 0, false).sound == TimerSound::None);
  CHECK("cue: no interval tick in the last minute", timerCue(50, 10, false).sound == TimerSound::None);
  {
    const TimerCue ten = timerCue(10, 0, false), one = timerCue(1, 0, false), five = timerCue(5, 0, false);
    CHECK("cue: countdown 10 -> 1 rises a semitone, volume 50 -> 100",
          ten.sound == TimerSound::Countdown && ten.beep.midi == 72 && ten.beep.volume == 50 && one.beep.midi == 81 &&
              one.beep.volume == 100 && five.beep.midi == 77 && five.beep.volume > ten.beep.volume &&
              five.beep.volume < one.beep.volume && timerCue(11, 0, false).sound == TimerSound::None);
  }
  CHECK("cue: 0 is the end", timerCue(0, 0, false).sound == TimerSound::End);
  CHECK("cue: busy skips ticks and countdown, and holds the end until the reply is over",
        timerCue(30, 0, true).sound == TimerSound::None && timerCue(5, 0, true).sound == TimerSound::None &&
            timerCue(0, 0, true).sound == TimerSound::None && timerCue(0, 0, false).sound == TimerSound::End);
  CHECK("refresh every 10 s, every second in the last minute",
        timerRefreshSeconds(300) == 10 && timerRefreshSeconds(61) == 10 && timerRefreshSeconds(60) == 1);
  return failures ? 1 : 0;
}
