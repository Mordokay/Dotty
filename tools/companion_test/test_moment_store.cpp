// uses: cartridges/companion/moment_store.cpp
// Scheduled moments (piece 3): the per-companion list of reminders, alarms and spontaneous lines,
// and the rules shared by every companion. Times are local epoch seconds.
#include <ctime>

#include "check.h"
#include "moment_store.h"

static int64_t local(int y, int mo, int d, int h, int mi) {
  tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  return timegm(&t);
}

static Moment reminder(const std::string &what, int64_t at) {
  Moment m;
  m.kind = MomentKind::Reminder;
  m.what = what;
  m.at = at;
  return m;
}

int main() {
  const int64_t fri0730 = local(2026, 10, 23, 7, 30);  // Friday 23 October 2026

  {
    MomentList l;
    const int a = l.add(reminder("one", fri0730)), b = l.add(reminder("two", fri0730));
    l.add(reminder("three", fri0730));
    l.remove(b);
    CHECK("add: ids grow, never reused", a == 1 && b == 2 && l.add(reminder("four", fri0730)) == 4);
  }
  {
    MomentList l;
    for (int i = 0; i < MomentList::kMax; i++) l.add(reminder("x", fri0730));
    CHECK_EQ("add: 50 max", l.add(reminder("one more", fri0730)), -1);
  }
  {
    MomentList l;
    Moment bad = reminder("ok", fri0730);
    bad.style = "shouted";
    CHECK("add: refuses empty what, bad style, at 0",
          l.add(reminder("", fri0730)) == -1 && l.add(bad) == -1 && l.add(reminder("ok", 0)) == -1 && l.all().empty());
  }
  {
    MomentList l;
    std::string text;
    while (text.size() < 260) text += "water the plants ";
    const int id = l.add(reminder(text, fri0730));
    const std::string &kept = l.find(id)->what;
    CHECK("what cut to 200 characters at a word", kept.size() <= 200 && kept.size() > 180 &&
                                                       kept.back() != ' ' && text.compare(0, kept.size() + 1, kept + " ") == 0);
  }
  {
    MomentList l;
    Moment a = reminder("cook dinner", fri0730);
    a.style = "sung";
    a.line = "Dinner time, my friend!";
    a.ready = true;
    a.created = fri0730 - 3600;
    Moment b;
    b.kind = MomentKind::Alarm;
    b.what = "gym";
    b.at = fri0730 + 86400;
    b.repeat = Repeat::Days;
    b.days = 0b0010101;
    b.source = "app";
    b.state = MomentState::Missed;
    l.add(a);
    l.add(b);
    MomentList back;
    const bool loaded = back.load(l.save());
    const Moment *x = back.find(1), *y = back.find(2);
    CHECK("save/load round trip", loaded && x && y && x->what == "cook dinner" && x->style == "sung" &&
                                      x->line == a.line && x->ready && x->created == a.created && x->at == fri0730 &&
                                      x->kind == MomentKind::Reminder && x->repeat == Repeat::Once &&
                                      y->kind == MomentKind::Alarm && y->repeat == Repeat::Days && y->days == 0b0010101 &&
                                      y->source == "app" && y->state == MomentState::Missed &&
                                      back.add(reminder("next", fri0730)) == 3);
  }
  {
    MomentList l;
    CHECK("load: broken json is false", !l.load("{x") && l.all().empty());
  }
  {
    MomentList l;
    Moment a = reminder("stretch", fri0730);
    a.ready = true;
    const int id = l.add(a);
    Moment moved = *l.find(id);
    moved.at += 600;
    l.update(moved);
    const bool keptReady = l.find(id)->ready;
    Moment changed = *l.find(id);
    changed.what = "stretch your back";
    l.update(changed);
    CHECK("update: changed what clears ready", keptReady && !l.find(id)->ready && l.find(id)->at == fri0730 + 600);
  }
  {
    int64_t t = 0, bad = 0;
    const bool ok = parseLocalTime("2026-10-24T18:00", t);
    CHECK("parseLocalTime", ok && t == local(2026, 10, 24, 18, 0) && !parseLocalTime("2026-13-01T10:00", bad) &&
                                !parseLocalTime("6pm", bad) && localTimeText(t) == "2026-10-24T18:00");
  }
  {
    Moment m = reminder("vitamins", fri0730);
    m.repeat = Repeat::Daily;
    CHECK_EQ("nextOccurrence: daily", nextOccurrence(m, local(2026, 10, 24, 8, 0)), local(2026, 10, 25, 7, 30));
    m.repeat = Repeat::Weekdays;
    CHECK_EQ("nextOccurrence: weekdays skip the weekend", nextOccurrence(m, local(2026, 10, 23, 7, 31)),
             local(2026, 10, 26, 7, 30));
    Moment d = reminder("swim", local(2026, 10, 19, 8, 0));  // Monday
    d.repeat = Repeat::Days;
    d.days = 0b0000101;  // Monday and Wednesday
    CHECK_EQ("nextOccurrence: chosen days", nextOccurrence(d, local(2026, 10, 19, 9, 0)), local(2026, 10, 21, 8, 0));
    CHECK_EQ("nextOccurrence: once in the past is 0", nextOccurrence(reminder("x", fri0730), fri0730 + 1), (int64_t)0);
  }
  {
    MomentRules r;
    CHECK("rules: defaults", r.perDay == 2 && r.from == 600 && r.to == 1260 && r.alarmVolume == 90 &&
                                 r.wakeEvery == 20 && !r.quietSilent && r.quiet.empty());
    MomentRules c;
    c.load(R"({"perDay":9,"alarmVolume":50,"wakeEvery":1})");
    MomentRules c2;
    c2.load(R"({"wakeEvery":999,"quietMode":"silent"})");
    CHECK("rules: clamps", c.perDay == 5 && c.alarmVolume == 80 && c.wakeEvery == 10 && c2.wakeEvery == 240 &&
                               c2.quietSilent);
    MomentRules n;
    n.quiet.push_back({0x7F, 23 * 60, 7 * 60});
    CHECK("rules: quiet window past midnight", n.inQuiet(local(2026, 10, 24, 2, 0)) && !n.inQuiet(local(2026, 10, 24, 12, 0)));
    MomentRules w;
    w.quiet.push_back({0b0011111, 9 * 60, 17 * 60});
    MomentRules back;
    back.load(w.save());
    CHECK("rules: quiet window on weekdays only", back.inQuiet(local(2026, 10, 19, 10, 0)) &&
                                                      !back.inQuiet(local(2026, 10, 24, 10, 0)));
    CHECK("rules: allowed hours", r.inAllowedHours(local(2026, 10, 24, 10, 0)) && !r.inAllowedHours(local(2026, 10, 24, 21, 0)) &&
                                      !r.inAllowedHours(local(2026, 10, 24, 9, 59)));
  }
  return failures ? 1 : 0;
}
