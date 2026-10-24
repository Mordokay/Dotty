#include "moment_schedule.h"

#include <algorithm>
#include <map>

Fate fate(int64_t at, int64_t now) {
  if (at > now) return Fate::Wait;
  return now - at < 3600 ? Fate::Fire : Fate::Missed;
}

Due due(const std::vector<CompanionMoments> &all, int64_t now, bool busy) {
  Due d;
  for (const CompanionMoments &c : all) {
    if (!c.list) continue;
    std::map<int64_t, FireGroup> byMinute;
    for (const Moment &m : c.list->all()) {
      if (m.state != MomentState::Upcoming || m.at <= 0) continue;
      const Fate f = fate(m.at, now);
      if (f == Fate::Missed) {
        d.missed.push_back({c.companion, m.id});
      } else if (f == Fate::Fire && !busy) {
        FireGroup &g = byMinute[m.at / 60];
        g.companion = c.companion;
        g.minute = m.at / 60;
        g.ids.push_back(m.id);
        if (m.kind == MomentKind::Alarm) g.alarm = true;
      }
    }
    for (auto &entry : byMinute) {
      std::sort(entry.second.ids.begin(), entry.second.ids.end());
      d.fire.push_back(entry.second);
    }
  }
  return d;
}

void advance(Moment &m, int64_t now, bool missed) {
  if (m.repeat != Repeat::Once) {
    const int64_t next = nextOccurrence(m, now);
    if (next) {
      m.at = next;
      m.state = MomentState::Upcoming;
      return;
    }
  }
  m.state = missed ? MomentState::Missed : MomentState::Done;
}

int64_t nextDue(const std::vector<CompanionMoments> &all) {
  int64_t best = 0;
  for (const CompanionMoments &c : all) {
    if (!c.list) continue;
    for (const Moment &m : c.list->all())
      if (m.state == MomentState::Upcoming && m.at > 0 && (!best || m.at < best)) best = m.at;
  }
  return best;
}

Delivery delivery(const MomentRules &rules, bool alarm, int64_t now) {
  if (alarm || !rules.inQuiet(now)) return Delivery::Voice;
  return rules.quietSilent ? Delivery::TextOnly : Delivery::BeepsAndText;
}

int groupWords(size_t topics) {
  const size_t words = 150 + 100 * (topics > 1 ? topics - 1 : 0);
  return static_cast<int>(std::min<size_t>(words, 300));
}

std::string groupSignature(const std::vector<const Moment *> &members) {
  std::vector<const Moment *> sorted = members;
  std::sort(sorted.begin(), sorted.end(), [](const Moment *a, const Moment *b) { return a->id < b->id; });
  std::string sig;
  for (const Moment *m : sorted)
    sig += std::to_string(m->id) + "|" + kindName(m->kind) + "|" + m->style + "|" + m->what + "\n";
  return sig;
}

std::vector<std::vector<int>> sharedMinutes(const MomentList &list) {
  std::map<int64_t, std::vector<int>> byMinute;
  for (const Moment &m : list.all())
    if (m.state == MomentState::Upcoming && m.at > 0) byMinute[m.at / 60].push_back(m.id);
  std::vector<std::vector<int>> out;
  for (auto &entry : byMinute) {
    if (entry.second.size() < 2) continue;
    std::sort(entry.second.begin(), entry.second.end());
    out.push_back(entry.second);
  }
  return out;
}
