// uses:
// Up to 3 Gemini keys, one per session in turn (the user's idea: each conversation stays on one
// key, the next session takes the next); a key out of allowance is skipped until its reset.
#include "check.h"
#include "key_ring.h"

int main() {
  KeyRing r;
  CHECK_EQ("no keys: nothing to pick", r.pick(100), -1);
  r.setCount(3);
  CHECK_EQ("first session: key 0", r.pick(100), 0);
  CHECK_EQ("next session: key 1", r.pick(100), 1);
  CHECK_EQ("then key 2", r.pick(100), 2);
  CHECK_EQ("and round again", r.pick(100), 0);
  r.markOut(1, 500);
  CHECK_EQ("a key that's out is skipped", r.pick(100), 2);
  CHECK_EQ("skipped again", r.pick(100), 0);
  CHECK_EQ("back after its reset", r.pick(600), 1);
  r.markOut(0, 900);
  r.markOut(1, 700);
  r.markOut(2, 800);
  CHECK_EQ("all out: none", r.pick(650), -1);
  CHECK_EQ("all out: until the first comes back", r.allOutUntil(650), int64_t(700));
  CHECK_EQ("not all out: 0", r.allOutUntil(750), int64_t(0));
  r.setCount(1);
  CHECK_EQ("fewer keys: the next wraps inside them", r.pick(1000), 0);
  CHECK_EQ("one key: always it", r.pick(1000), 0);
  KeyRing s;
  s.setCount(2);
  s.restore(1, {0, 400});
  CHECK_EQ("restored: the saved next and its out time", s.pick(100), 0);
  CHECK_EQ("restored: next", s.next(), 1);
  // Strikes are per key: a good turn on another key doesn't forgive the one that's out for the day.
  KeyRing q;
  q.setCount(3);
  CHECK_EQ("strikes: key 0's first", q.strike(0), 1);
  q.forgive(1);  // a good turn on key 1
  CHECK_EQ("strikes: key 0's second still counts", q.strike(0), 2);
  q.forgive(0);
  CHECK_EQ("strikes: a good turn on key 0 starts it over", q.strike(0), 1);
  q.setCount(2);
  CHECK_EQ("strikes: kept per key when the count changes", q.strike(0), 2);
  return failures ? 1 : 0;
}
