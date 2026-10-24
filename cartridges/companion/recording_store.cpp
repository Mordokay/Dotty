#include "recording_store.h"

#include <ArduinoJson.h>

#include <cctype>
#include <cstdio>
#include <ctime>

#include "civil_time.h"

namespace {

tm parts(int64_t local) {
  const time_t t = static_cast<time_t>(local);
  tm out;
  gmtime_r(&t, &out);
  return out;
}

int64_t dayOf(int64_t local) {
  return local - ((local % 86400) + 86400) % 86400;
}

std::string lower(std::string s) {
  for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> words(const std::string &s) {
  std::vector<std::string> out;
  std::string w;
  for (const char c : lower(s) + " ") {
    if (std::isalnum(static_cast<unsigned char>(c)) || (c & 0x80)) {
      w += c;
    } else {
      if (w.size() >= 3) out.push_back(w);
      w.clear();
    }
  }
  return out;
}

bool stopWord(const std::string &w) {
  static const char *kStop[] = {"the", "one", "recording", "recordings", "about", "from", "that", "with", "and",
                                "play", "please", "can", "you", "for", "this", "note", "voice", "made"};
  for (const char *s : kStop)
    if (w == s) return true;
  return false;
}

// A day the query names: yesterday, today, YYYY-MM-DD, "24 October" / "October 24". 0 = none.
int64_t queryDay(const std::string &q, int64_t now) {
  if (q.find("yesterday") != std::string::npos) return dayOf(now) - 86400;
  if (q.find("today") != std::string::npos) return dayOf(now);
  int y = 0, m = 0, d = 0;
  for (size_t i = 0; i + 10 <= q.size(); i++)
    if (std::sscanf(q.c_str() + i, "%4d-%2d-%2d", &y, &m, &d) == 3 && y > 2000) {
      return civilEpoch(y, m, d);
    }
  static const char *kMonths[] = {"january", "february", "march", "april", "may", "june", "july",
                                  "august", "september", "october", "november", "december"};
  for (int mi = 0; mi < 12; mi++) {
    const size_t at = q.find(kMonths[mi]);
    if (at == std::string::npos) continue;
    int day = 0;
    if (std::sscanf(q.c_str() + at + std::string(kMonths[mi]).size(), " %d", &day) != 1) {  // "October 24"
      size_t s = at;
      while (s > 0 && q[s - 1] == ' ') s--;
      size_t b = s;
      while (b > 0 && std::isdigit(static_cast<unsigned char>(q[b - 1]))) b--;
      if (b < s) day = std::atoi(q.c_str() + b);  // "24 October"
    }
    if (day < 1 || day > 31) continue;
    return civilEpoch(parts(now).tm_year + 1900, mi + 1, day);
  }
  return 0;
}

}  // namespace

std::string recordingName(int64_t localEpoch) {
  const tm t = parts(localEpoch);
  char buf[20];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                t.tm_min, t.tm_sec);
  return buf;
}

bool parseRecordingMeta(const std::string &json, RecordingMeta &out) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok || !doc.is<JsonObjectConst>()) return false;
  RecordingMeta m;
  m.name = doc["name"] | "";
  m.title = doc["title"] | "";
  m.created = doc["created"] | (int64_t)0;
  m.ms = doc["ms"] | 0u;
  m.parts = doc["parts"] | 0;
  m.synced = doc["synced"] | false;
  m.isDraft = doc["isDraft"] | false;
  m.textAt = doc["textAt"] | (int64_t)0;
  m.summary = doc["summary"] | "";
  for (JsonVariantConst p : doc["points"].as<JsonArrayConst>()) m.points.push_back(p | "");
  out = m;
  return true;
}

std::string recordingMetaJson(const RecordingMeta &m) {
  JsonDocument doc;
  doc["name"] = m.name;
  doc["title"] = m.title;
  doc["created"] = m.created;
  doc["ms"] = m.ms;
  doc["parts"] = m.parts;
  doc["synced"] = m.synced;
  doc["isDraft"] = m.isDraft;
  doc["textAt"] = m.textAt;
  doc["summary"] = m.summary;
  JsonArray points = doc["points"].to<JsonArray>();
  for (const std::string &p : m.points) points.add(p);
  std::string out;
  serializeJson(doc, out);
  return out;
}

int matchRecording(const std::vector<RecordingMeta> &newestFirst, const std::string &query, int64_t now) {
  if (newestFirst.empty()) return -1;
  const std::string q = lower(query);
  for (const char *w : {"last", "latest", "newest", "most recent"})
    if (q.find(w) != std::string::npos) return 0;
  const int64_t day = queryDay(q, now);
  if (day) {
    for (size_t i = 0; i < newestFirst.size(); i++)
      if (dayOf(newestFirst[i].created) == day) return static_cast<int>(i);
    return -1;
  }
  int best = -1, bestScore = 0;
  for (size_t i = 0; i < newestFirst.size(); i++) {
    const std::string title = lower(newestFirst[i].title), summary = lower(newestFirst[i].summary);
    int score = 0;
    for (const std::string &w : words(q)) {
      if (stopWord(w)) continue;
      if (title.find(w) != std::string::npos) score += 2;
      if (summary.find(w) != std::string::npos) score += 1;
    }
    if (score > bestScore) {  // ties: the newer one (earlier in the list)
      best = static_cast<int>(i);
      bestScore = score;
    }
  }
  return best;
}

std::string recordingsSection(const std::vector<RecordingMeta> &newestFirst, int max) {
  if (newestFirst.empty()) return "";
  static const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  std::string s = "Voice notes the user recorded with you (newest first; recall reads their full text):";
  for (size_t i = 0; i < newestFirst.size() && static_cast<int>(i) < max; i++) {
    const RecordingMeta &m = newestFirst[i];
    const tm t = parts(m.created);
    s += "\n- " + std::to_string(t.tm_mday) + " " + kMonths[t.tm_mon] + ", '" + m.title + "'";
    if (!m.summary.empty()) s += ": " + m.summary;
  }
  return s;
}

std::string transcriptBlock(const RecordingMeta &m, const std::string &transcript) {
  static const char *kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const tm t = parts(m.created);
  char when[32];
  std::snprintf(when, sizeof(when), "%s %d %s %02d:%02d", kDays[t.tm_wday], t.tm_mday, kMonths[t.tm_mon], t.tm_hour, t.tm_min);
  return std::string(when) + " - Recording '" + m.title + "': " + transcript;
}

HeaderFix headerFix(uint32_t fileBytes, uint32_t headerDataBytes) {
  HeaderFix f;
  if (fileBytes < 44) return f;
  f.dataBytes = (fileBytes - 44) & ~1u;
  f.needed = f.dataBytes != headerDataBytes;
  return f;
}
