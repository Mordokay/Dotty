// uses: cartridges/companion/moment_schedule.cpp cartridges/companion/moment_store.cpp
// The moments scheduler: which moments fire now (grouped per companion and minute), which were missed,
// quiet windows, the line's word budget, and an alarm ringing with its snoozes.
#include <ctime>

#include "check.h"
#include "moment_schedule.h"

static int64_t local(int y, int mo, int d, int h, int mi, int s = 0) {
  tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_sec = s;
  return timegm(&t);
}

static Moment make(MomentKind kind, const std::string &what, int64_t at, Repeat repeat = Repeat::Once) {
  Moment m;
  m.kind = kind;
  m.what = what;
  m.at = at;
  m.repeat = repeat;
  return m;
}

int main() {
  const int64_t six = local(2026, 10, 24, 18, 0);

  CHECK("fate: 59 minutes late fires, 61 is missed, future waits",
        fate(six, six + 59 * 60) == Fate::Fire && fate(six, six + 61 * 60) == Fate::Missed && fate(six, six - 1) == Fate::Wait);

  {
    MomentList friendList, coachList;
    friendList.add(make(MomentKind::Reminder, "cook dinner", six));
    friendList.add(make(MomentKind::Reminder, "call mum", six + 30));  // the same minute
    coachList.add(make(MomentKind::Reminder, "stretch", six));
    const std::vector<CompanionMoments> all = {{"friend", &friendList}, {"coach", &coachList}};
    const Due d = due(all, six + 40, false);
    CHECK("due: two of a companion at one minute are one group",
          d.fire.size() == 2 && d.fire[0].ids.size() == 2 && d.fire[0].ids[0] == 1 && d.fire[0].ids[1] == 2);
    CHECK("due: other companions apart, in companion order",
          d.fire.size() == 2 && d.fire[0].companion == "friend" && d.fire[1].companion == "coach" && !d.fire[0].alarm);
    CHECK("busy: nothing fires while busy", due(all, six + 40, true).fire.empty());
  }
  {
    MomentList l;
    l.add(make(MomentKind::Reminder, "pills", six));
    l.add(make(MomentKind::Alarm, "leave", six));
    const std::vector<CompanionMoments> all = {{"friend", &l}};
    const Due d = due(all, six, false);
    CHECK("due: the alarm makes the group an alarm", d.fire.size() == 1 && d.fire[0].alarm && d.fire[0].ids.size() == 2);
  }
  {
    MomentList l;
    const int id = l.add(make(MomentKind::Reminder, "water", six));
    const std::vector<CompanionMoments> all = {{"friend", &l}};
    const Due first = due(all, six + 30 * 60, false);
    advance(*l.find(id), six + 30 * 60, false);
    CHECK("clock jump: forward 30 minutes past a moment fires it once",
          first.fire.size() == 1 && due(all, six + 31 * 60, false).fire.empty() && l.find(id)->state == MomentState::Done);
  }
  {
    MomentList l;
    l.add(make(MomentKind::Reminder, "water", six));
    const std::vector<CompanionMoments> all = {{"friend", &l}};
    const Due d = due(all, six + 2 * 3600, false);
    CHECK("clock jump: forward 2 hours makes it missed",
          d.fire.empty() && d.missed.size() == 1 && d.missed[0].first == "friend" && d.missed[0].second == 1);
  }
  {
    MomentList l;
    const int id = l.add(make(MomentKind::Reminder, "vitamins", six, Repeat::Daily));
    const std::vector<CompanionMoments> all = {{"friend", &l}};
    advance(*l.find(id), six + 5, false);  // fired at 18:00:05
    CHECK("clock jump: backwards doesn't fire it again",
          due(all, six - 600, false).fire.empty() && due(all, six + 60, false).fire.empty() &&
              l.find(id)->at == six + 86400);
  }
  {
    MomentList l;
    l.add(make(MomentKind::Reminder, "water", six));
    const std::vector<CompanionMoments> all = {{"friend", &l}};
    CHECK("early wake: a wake 2 s before the minute doesn't skip it",
          due(all, six - 2, false).fire.empty() && due(all, six + 1, false).fire.size() == 1);
    CHECK_EQ("nextDue: the earliest", nextDue(all), six);
  }
  {
    Moment once = make(MomentKind::Reminder, "x", six), daily = make(MomentKind::Reminder, "y", six, Repeat::Daily);
    daily.ready = true;
    advance(once, six, false);
    advance(daily, six, false);
    Moment late = make(MomentKind::Reminder, "z", six);
    advance(late, six + 7200, true);
    CHECK("advance: repeating moves on, one-off is done",
          once.state == MomentState::Done && daily.state == MomentState::Upcoming && daily.at == six + 86400 &&
              daily.ready && late.state == MomentState::Missed);
  }
  {
    MomentRules beeps, silent;
    beeps.quiet.push_back({0x7F, 17 * 60, 19 * 60});
    silent = beeps;
    silent.quietSilent = true;
    CHECK("delivery: quiet reminder beeps or text only; alarm always voice",
          delivery(beeps, false, six) == Delivery::BeepsAndText && delivery(silent, false, six) == Delivery::TextOnly &&
              delivery(silent, true, six) == Delivery::Voice && delivery(MomentRules(), false, six) == Delivery::Voice);
  }
  CHECK("groupWords: 150, 250, 300, 300 for 1-4 topics",
        groupWords(1) == 150 && groupWords(2) == 250 && groupWords(3) == 300 && groupWords(4) == 300);
  {
    Moment a = make(MomentKind::Reminder, "pills", six), b = make(MomentKind::Alarm, "leave", six);
    a.id = 1;
    b.id = 2;
    const std::string sig = groupSignature({&a, &b});
    Moment moved = a;
    moved.at += 3600;
    Moment restyled = a;
    restyled.style = "sung";
    Moment reworded = a;
    reworded.what = "pills and water";
    CHECK("groupSignature changes with what or style, not with at",
          groupSignature({&moved, &b}) == sig && groupSignature({&restyled, &b}) != sig &&
              groupSignature({&reworded, &b}) != sig && groupSignature({&b, &a}) == sig);
  }
  {
    MomentList l;
    l.add(make(MomentKind::Reminder, "pills", six));
    l.add(make(MomentKind::Alarm, "leave", six + 20));
    l.add(make(MomentKind::Reminder, "alone", six + 3600));
    const std::vector<std::vector<int>> shared = sharedMinutes(l);
    CHECK("sharedMinutes: two at one minute make a group, one alone doesn't",
          shared.size() == 1 && shared[0].size() == 2 && shared[0][0] == 1 && shared[0][1] == 2);
  }
  {
    Ringing r;
    r.start(0, true);
    const bool plays = r.poll(0) == Ringing::Action::Play && r.poll(30000) == Ringing::Action::None &&
                       r.poll(60000) == Ringing::Action::Play && r.poll(120000) == Ringing::Action::Play &&
                       r.poll(540000) == Ringing::Action::Play;
    CHECK("ringing: plays at 0, 60, 120 s; gives up at 600 s",
          plays && r.poll(600000) == Ringing::Action::GiveUp && !r.active());
  }
  {
    Ringing r;
    r.start(0, true);
    r.poll(0);
    const bool three = r.snooze(1000) && r.snooze(2000) && r.snooze(3000);
    CHECK("ringing: three snoozes, the fourth stops", three && !r.snooze(4000) && !r.active() && r.snoozes() == 3);
  }
  {
    Ringing r;
    r.start(0, true);
    r.poll(0);
    r.snooze(10000);
    const uint32_t back = 10000 + Ringing::kSnoozeMs;
    CHECK("ringing: a snooze rings again 5 minutes later and restarts the 10 minutes",
          r.snoozed() && r.poll(200000) == Ringing::Action::None && r.poll(back) == Ringing::Action::Play &&
              !r.snoozed() && r.poll(back + 599000) != Ringing::Action::GiveUp &&
              r.poll(back + 600000) == Ringing::Action::GiveUp);
  }
  {
    Ringing r;
    r.start(0, false);
    r.poll(0);
    CHECK("ringing: not snoozable (timer) -> snooze stops it", !r.snooze(1000) && !r.active());
  }
  return failures ? 1 : 0;
}
