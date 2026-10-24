// The phone wake: a locked Dotty turns Bluetooth on for a while so the iPhone can reconnect and run the
// due jobs (recording lines, the daily plan; later the phone's bundle). 30 s for the phone, 3 minutes
// for the jobs, Wi-Fi when the phone doesn't come — and Bluetooth always off at the end.
#include "check.h"
#include "phone_wake.h"

int main() {
  PhoneWake w;
  CHECK("nextWakeAt: nothing due -> 0; due -> last + 20 minutes",
        w.nextWakeAt(1000, 20, false) == 0 && w.nextWakeAt(1000, 20, true) == 1000 + 1200);
  {
    PhoneWake p;
    p.start(0);
    const bool waiting = p.poll(1000, false, true, false) == PhoneWake::Stage::WaitingForPhone && p.bluetoothOn();
    CHECK("phone in 5 s -> Phone, Bluetooth on",
          waiting && p.poll(5000, true, true, false) == PhoneWake::Stage::Phone && p.bluetoothOn());
  }
  {
    PhoneWake p;
    p.start(0);
    p.poll(29000, false, true, false);
    CHECK("no phone in 30 s, Wi-Fi jobs -> Wifi, Bluetooth off",
          p.poll(30000, false, true, false) == PhoneWake::Stage::Wifi && !p.bluetoothOn());
  }
  {
    PhoneWake p;
    p.start(0);
    CHECK("no phone, no wifi: Done at 30 s, Bluetooth off",
          p.poll(29999, false, false, false) == PhoneWake::Stage::WaitingForPhone &&
              p.poll(30000, false, false, false) == PhoneWake::Stage::Done && !p.bluetoothOn());
  }
  {
    PhoneWake p;
    p.start(0);
    p.poll(2000, true, true, false);
    CHECK("jobs done -> Done at once", p.poll(9000, true, true, true) == PhoneWake::Stage::Done && !p.bluetoothOn());
  }
  {
    PhoneWake p;
    p.start(0);
    p.poll(5000, true, true, false);
    CHECK("jobs over 3 minutes -> Done",
          p.poll(184000, true, true, false) == PhoneWake::Stage::Phone &&
              p.poll(185000, true, true, false) == PhoneWake::Stage::Done);
  }
  {
    PhoneWake p;
    p.start(0);
    p.poll(30000, false, true, false);
    CHECK("wifi jobs over 3 minutes -> Done", p.poll(210000, false, true, false) == PhoneWake::Stage::Done);
  }
  {
    PhoneWake p;
    p.start(0);
    p.poll(1000, true, true, false);
    p.end();
    CHECK("end() -> Idle, Bluetooth off", p.stage() == PhoneWake::Stage::Idle && !p.bluetoothOn() &&
                                              p.poll(2000, true, true, false) == PhoneWake::Stage::Idle);
  }
  CHECK("jobs done: nothing left", PhoneWake::jobsDone(false, false, false, false, 0));
  CHECK("jobs done: a line or a plan left", !PhoneWake::jobsDone(true, false, false, false, 0) &&
                                                !PhoneWake::jobsDone(false, false, false, true, 0));
  CHECK("two failures end the wake even with a plan due (no internet: don't sit 3 minutes awake)",
        PhoneWake::jobsDone(true, false, false, true, 2));
  CHECK("a job still running isn't cut by the failures", !PhoneWake::jobsDone(true, true, false, true, 2) &&
                                                            !PhoneWake::jobsDone(false, false, true, false, 2));
  return failures ? 1 : 0;
}
