#include "moment_store.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace {

std::string trim(const std::string &s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// At most kWhatMax characters (code points), cut back to a word.
std::string cutWhat(const std::string &text) {
  const std::string t = trim(text);
  size_t points = 0, end = 0;
  for (size_t i = 0; i < t.size(); i++) {
    if ((static_cast<unsigned char>(t[i]) & 0xC0) == 0x80) continue;
    if (points == static_cast<size_t>(MomentList::kWhatMax)) {
      end = i;
      break;
    }
    points++;
  }
  if (points < static_cast<size_t>(MomentList::kWhatMax) || end == 0) return t;
  std::string head = t.substr(0, end);
  if (t[end] != ' ') {
    const size_t space = head.find_last_of(' ');
    if (space != std::string::npos) head = head.substr(0, space);
  }
  return trim(head);
}

const char *stateName(MomentState s) {
  return s == MomentState::Done ? "done" : s == MomentState::Missed ? "missed" : "upcoming";
}

MomentState stateFromName(const std::string &n) {
  return n == "done" ? MomentState::Done : n == "missed" ? MomentState::Missed : MomentState::Upcoming;
}

int64_t dayStart(int64_t local) {
  return local - ((local % 86400) + 86400) % 86400;
}

int minuteOfDay(int64_t local) {
  return static_cast<int>((((local % 86400) + 86400) % 86400) / 60);
}

uint8_t repeatDays(const Moment &m) {
  switch (m.repeat) {
    case Repeat::Daily: return 0x7F;
    case Repeat::Weekdays: return 0b0011111;
    case Repeat::Days: return m.days & 0x7F;
    default: return 0;
  }
}

// Seconds since 1970 for a calendar date and time, with no zone (newlib has no timegm). Days from the
// civil date: Howard Hinnant's algorithm.
int64_t civilEpoch(int y, int m, int d, int h, int mi, int s) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + doe - 719468;
  return days * 86400 + h * 3600 + mi * 60 + s;
}

int clampInt(int v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

}  // namespace

int weekdayBit(int64_t local) {
  const time_t t = static_cast<time_t>(local);
  tm parts;
  gmtime_r(&t, &parts);
  return 1 << ((parts.tm_wday + 6) % 7);  // tm_wday: 0 = Sunday
}

const char *kindName(MomentKind k) {
  return k == MomentKind::Alarm ? "alarm" : k == MomentKind::Spontaneous ? "spontaneous" : "reminder";
}

bool kindFromName(const std::string &name, MomentKind &out) {
  if (name == "reminder") out = MomentKind::Reminder;
  else if (name == "alarm") out = MomentKind::Alarm;
  else if (name == "spontaneous") out = MomentKind::Spontaneous;
  else return false;
  return true;
}

const char *repeatName(Repeat r) {
  return r == Repeat::Daily ? "daily" : r == Repeat::Weekdays ? "weekdays" : r == Repeat::Days ? "days" : "once";
}

bool repeatFromName(const std::string &name, Repeat &out) {
  if (name == "once" || name.empty()) out = Repeat::Once;
  else if (name == "daily") out = Repeat::Daily;
  else if (name == "weekdays") out = Repeat::Weekdays;
  else if (name == "days") out = Repeat::Days;
  else return false;
  return true;
}

bool validStyle(const std::string &style) {
  return style == "spoken" || style == "sung" || style == "whispered" || style == "excited" || style == "calm";
}

bool parseLocalTime(const std::string &iso, int64_t &local) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  const int n = std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s);
  if (n < 5 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) return false;
  const time_t v = static_cast<time_t>(civilEpoch(y, mo, d, h, mi, s));
  tm back;
  gmtime_r(&v, &back);
  if (back.tm_mday != d || back.tm_mon != mo - 1) return false;  // 31 April
  local = v;
  return true;
}

std::string localTimeText(int64_t local) {
  const time_t t = static_cast<time_t>(local);
  tm parts;
  gmtime_r(&t, &parts);
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                parts.tm_hour, parts.tm_min);
  return buf;
}

int64_t nextOccurrence(const Moment &m, int64_t after) {
  if (m.repeat == Repeat::Once) return m.at > after ? m.at : 0;
  const uint8_t days = repeatDays(m);
  if (!days || m.at <= 0) return 0;
  const int64_t base = std::max(after + 1, m.at);
  const int64_t timeOfDay = m.at - dayStart(m.at);
  for (int i = 0; i <= 7; i++) {
    const int64_t candidate = dayStart(base) + i * 86400LL + timeOfDay;
    if (candidate >= base && (weekdayBit(candidate) & days)) return candidate;
  }
  return 0;
}

bool firstOccurrence(Moment &m) {
  if (m.repeat == Repeat::Once) return m.at > 0;
  const int64_t first = nextOccurrence(m, m.at - 1);
  if (!first) return false;
  m.at = first;
  return true;
}

const Moment *MomentList::find(int id) const {
  for (const Moment &m : moments_)
    if (m.id == id) return &m;
  return nullptr;
}

Moment *MomentList::find(int id) {
  for (Moment &m : moments_)
    if (m.id == id) return &m;
  return nullptr;
}

int MomentList::add(Moment m) {
  m.what = cutWhat(m.what);
  if (static_cast<int>(moments_.size()) >= kMax || m.what.empty() || m.at <= 0 || !validStyle(m.style)) return -1;
  m.id = next_++;
  moments_.push_back(m);
  return m.id;
}

bool MomentList::update(const Moment &m) {
  Moment *old = find(m.id);
  const std::string what = cutWhat(m.what);
  if (!old || what.empty() || m.at <= 0 || !validStyle(m.style)) return false;
  const bool newLine = what != old->what || m.style != old->style || m.kind != old->kind;
  Moment next = m;
  next.what = what;
  if (newLine) {
    next.ready = false;
    next.line.clear();
  }
  *old = next;
  return true;
}

bool MomentList::remove(int id) {
  const auto it = std::find_if(moments_.begin(), moments_.end(), [id](const Moment &m) { return m.id == id; });
  if (it == moments_.end()) return false;
  moments_.erase(it);
  return true;
}

bool MomentList::load(const std::string &json) {
  moments_.clear();
  next_ = 1;
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok || !doc["moments"].is<JsonArrayConst>()) return false;
  int highest = 0;
  for (JsonObjectConst o : doc["moments"].as<JsonArrayConst>()) {
    Moment m;
    m.id = o["id"] | 0;
    if (m.id <= 0) continue;
    kindFromName(o["kind"] | "reminder", m.kind);
    m.at = o["at"] | (int64_t)0;
    repeatFromName(o["repeat"] | "once", m.repeat);
    m.days = static_cast<uint8_t>((o["days"] | 0) & 0x7F);
    m.what = o["what"] | "";
    m.style = o["style"] | "spoken";
    if (!validStyle(m.style)) m.style = "spoken";
    m.source = o["source"] | "chat";
    m.line = o["line"] | "";
    m.ready = o["ready"] | false;
    m.state = stateFromName(o["state"] | "upcoming");
    m.created = o["created"] | (int64_t)0;
    if (m.what.empty()) continue;
    moments_.push_back(m);
    highest = std::max(highest, m.id);
  }
  next_ = std::max(doc["next"] | 1, highest + 1);
  return true;
}

std::string MomentList::save() const {
  JsonDocument doc;
  doc["next"] = next_;
  JsonArray list = doc["moments"].to<JsonArray>();
  for (const Moment &m : moments_) {
    JsonObject o = list.add<JsonObject>();
    o["id"] = m.id;
    o["kind"] = kindName(m.kind);
    o["at"] = m.at;
    o["repeat"] = repeatName(m.repeat);
    if (m.repeat == Repeat::Days) o["days"] = m.days;
    o["what"] = m.what;
    o["style"] = m.style;
    o["source"] = m.source;
    if (!m.line.empty()) o["line"] = m.line;
    o["ready"] = m.ready;
    o["state"] = stateName(m.state);
    o["created"] = m.created;
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool MomentRules::load(const std::string &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok || !doc.is<JsonObjectConst>()) return false;
  perDay = clampInt(doc["perDay"] | perDay, 0, 5);
  from = clampInt(doc["from"] | from, 0, 1439);
  to = clampInt(doc["to"] | to, 0, 1439);
  if (doc["quiet"].is<JsonArrayConst>()) {
    quiet.clear();
    for (JsonObjectConst q : doc["quiet"].as<JsonArrayConst>()) {
      QuietWindow w;
      w.days = static_cast<uint8_t>((q["days"] | 0x7F) & 0x7F);
      w.from = clampInt(q["from"] | 0, 0, 1439);
      w.to = clampInt(q["to"] | 0, 0, 1439);
      if (w.from != w.to && w.days) quiet.push_back(w);
    }
  }
  if (doc["quietMode"].is<const char *>()) quietSilent = std::string(doc["quietMode"].as<const char *>()) == "silent";
  alarmVolume = clampInt(doc["alarmVolume"] | alarmVolume, 80, 100);
  wakeEvery = clampInt(doc["wakeEvery"] | wakeEvery, 10, 240);
  return true;
}

std::string MomentRules::save() const {
  JsonDocument doc;
  doc["perDay"] = perDay;
  doc["from"] = from;
  doc["to"] = to;
  JsonArray q = doc["quiet"].to<JsonArray>();
  for (const QuietWindow &w : quiet) {
    JsonObject o = q.add<JsonObject>();
    o["days"] = w.days;
    o["from"] = w.from;
    o["to"] = w.to;
  }
  doc["quietMode"] = quietSilent ? "silent" : "beeps";
  doc["alarmVolume"] = alarmVolume;
  doc["wakeEvery"] = wakeEvery;
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool MomentRules::inQuiet(int64_t local) const {
  const int minute = minuteOfDay(local);
  const int today = weekdayBit(local), yesterday = weekdayBit(local - 86400);
  for (const QuietWindow &w : quiet) {
    if (w.from < w.to) {
      if ((w.days & today) && minute >= w.from && minute < w.to) return true;
    } else if (((w.days & today) && minute >= w.from) || ((w.days & yesterday) && minute < w.to)) {
      return true;
    }
  }
  return false;
}

bool MomentRules::inAllowedHours(int64_t local) const {
  const int minute = minuteOfDay(local);
  return from <= to ? minute >= from && minute < to : minute >= from || minute < to;
}
