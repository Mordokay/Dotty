// The recording deck (recordings spec, "Recording on Dotty"): BOOT held = record, released = pause; Undo,
// Done, Discard (asked first), the back arrow; locking = Done; the limits (2 minutes with nothing, 5 minutes
// paused, a warning at 29:30 and Done at 30:00 of recording).
#include "check.h"
#include "note_deck.h"

using E = NoteDeck::Event;
using S = NoteDeck::State;

int main() {
  {
    NoteDeck d;
    d.open(0);
    const bool waiting = d.state() == S::Waiting;
    const E a = d.hold(true, 1000), b = d.hold(false, 4000), c = d.hold(true, 10000), e = d.hold(false, 12000);
    CHECK("hold records, release pauses, parts count", waiting && a == E::PartStart && b == E::PartEnd && c == E::PartStart &&
                                                           e == E::PartEnd && d.parts() == 2 && d.state() == S::Paused);
    CHECK_EQ("elapsed counts only recording", d.elapsedMs(50000), (uint32_t)5000);
    CHECK_EQ("lastPartMs", d.lastPartMs(), (uint32_t)2000);
    const E u = d.undo();
    CHECK("undo drops the last part", u == E::Undo && d.parts() == 1 && d.elapsedMs(60000) == 3000 && d.lastPartMs() == 3000);
    d.undo();
    CHECK("undo with nothing: None", d.undo() == E::None && d.parts() == 0);
  }
  {
    NoteDeck d;
    d.open(0);
    CHECK("done with nothing -> CloseEmpty", d.done() == E::CloseEmpty && d.state() == S::Closed);
  }
  {
    NoteDeck d;
    d.open(0);
    d.hold(true, 0);
    d.hold(false, 1000);
    CHECK("done -> Done", d.done() == E::Done && d.state() == S::Closed);
  }
  {
    NoteDeck d;
    d.open(0);
    d.hold(true, 0);
    d.hold(false, 1000);
    const E first = d.discard(false);
    const bool asking = d.state() == S::ConfirmDiscard;
    d.keep();
    const bool back = d.state() == S::Paused;
    d.discard(false);
    CHECK("discard asks first, then Discard", first == E::None && asking && back && d.discard(true) == E::Discard &&
                                                   d.state() == S::Closed);
  }
  {
    NoteDeck empty, some;
    empty.open(0);
    some.open(0);
    some.hold(true, 0);
    some.hold(false, 500);
    CHECK("cancel with nothing closes, with something asks",
          empty.cancel() == E::CloseEmpty && some.cancel() == E::None && some.state() == S::ConfirmDiscard);
  }
  {
    NoteDeck d;
    d.open(0);
    CHECK("2 minutes waiting -> CloseEmpty", d.tick(119999) == E::None && d.tick(120000) == E::CloseEmpty);
  }
  {
    NoteDeck d;
    d.open(0);
    d.hold(true, 0);
    d.hold(false, 1000);
    CHECK("5 minutes paused -> Done", d.tick(300999) == E::None && d.tick(301000) == E::Done);
  }
  {
    NoteDeck d;
    d.open(0);
    d.hold(true, 0);
    const E before = d.tick(1769999), warn = d.tick(1770000), again = d.tick(1770500), end = d.tick(1800000);
    CHECK("warn at 29:30 once, Done at 30:00 (recording)",
          before == E::None && warn == E::Warn && again == E::None && end == E::Done && d.state() == S::Closed);
  }
  {
    NoteDeck d;
    d.open(0);
    d.hold(true, 0);
    CHECK("lock while recording -> Done", d.lockOrPower(5000) == E::Done && d.parts() == 1 && d.elapsedMs(9000) == 5000);
  }
  {
    NoteDeck d;
    d.open(0);
    d.hold(true, 0);
    CHECK("phone gone mid-recording (nothing to do with the deck)", d.tick(60000) == E::None && d.state() == S::Recording);
  }
  {
    NoteDeck d;
    d.open(0);
    d.hold(true, 0);
    d.hold(false, 1000);
    d.discard(false);
    CHECK("BOOT on the discard question records again", d.hold(true, 2000) == E::PartStart && d.state() == S::Recording);
  }
  return failures ? 1 : 0;
}
