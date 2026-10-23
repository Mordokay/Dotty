// uses:
// The writer's background job (cartridges/companion/job_slot.h): each job owns its request; a
// result reaches the loop only if nobody gave up on the job meanwhile; one job at a time.
#include <string>

#include "check.h"
#include "job_slot.h"

int main() {
  JobSlot<std::string> slot;
  CHECK("idle", !slot.busy());
  const uint32_t a = slot.start();
  CHECK("started: busy", slot.busy());
  std::string out;
  CHECK("nothing yet", !slot.take(out));
  slot.finish(a, "written");
  CHECK("finished: idle", !slot.busy());
  CHECK("its result reaches the loop", slot.take(out) && out == "written");
  CHECK("taken once", !slot.take(out));

  const uint32_t b = slot.start();
  slot.cancel();  // BOOT: the user asked something else
  CHECK("cancelled but still running: busy (no second request on top of Live)", slot.busy());
  slot.finish(b, "stale");
  CHECK("a cancelled job's result is dropped", !slot.take(out) && !slot.busy());

  const uint32_t c = slot.start();
  slot.cancel();
  slot.finish(c, "stale");
  const uint32_t d = slot.start();
  CHECK("a stale result left over doesn't land on the next job", !slot.take(out));
  slot.finish(d, "fresh");
  CHECK("the next job's own result does", slot.take(out) && out == "fresh");
  return failures ? 1 : 0;
}
