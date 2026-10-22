// uses:
// Long fields and photos arrive in pieces (BLE commands ≤ 512 bytes): kept aside until complete.
#include "check.h"
#include "piece_buffer.h"

int main() {
  PieceBuffer b;
  b.begin(10);
  CHECK("first piece", b.add(0, "hello") && !b.complete());
  CHECK("second piece completes", b.add(5, "world") && b.complete() && b.data() == "helloworld");
  b.begin(6);
  CHECK("out of order refused", !b.add(3, "abc") && !b.complete());
  CHECK("in order again", b.add(0, "abc") && b.add(3, "def") && b.complete());
  b.begin(4);
  CHECK("past the end refused", !b.add(0, "abcde") && !b.complete());
  b.begin(4);
  b.add(0, "ab");
  b.clear();
  CHECK("clear drops a half buffer", !b.complete() && b.data().empty());
  return failures ? 1 : 0;
}
