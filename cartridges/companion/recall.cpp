#include "recall.h"

#include <ArduinoJson.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "companion_store.h"

namespace {

constexpr int64_t kDay = 86400;
constexpr const char *kWeekdays[] = {"Thursday", "Friday", "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday"};
constexpr const char *kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};

int64_t floorDiv(int64_t a, int64_t b) {
  return a / b - (a % b != 0 && (a < 0) != (b < 0));
}

// Howard Hinnant's civil-from-days and back.
void civil(int64_t z, int &y, int &m, int &d) {
  z += 719468;
  const int64_t era = floorDiv(z, 146097);
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yoe + era * 400 + (m <= 2));
}

int64_t daysFrom(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = floorDiv(y, 400);
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

bool parseDay(const std::string &s, int64_t &days) {
  int y, m, d;
  if (s.size() != 10 || sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return false;
  days = daysFrom(y, m, d);
  return true;
}

std::string dayName(int64_t days) {
  int y, m, d;
  civil(days, y, m, d);
  char buf[12];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
  return buf;
}

// "Thu 22 Oct 09:14"
std::string shortStamp(int64_t local) {
  const int64_t days = floorDiv(local, kDay), secs = local - days * kDay;
  int y, m, d;
  civil(days, y, m, d);
  char buf[32];
  snprintf(buf, sizeof(buf), "%.3s %d %.3s %02d:%02d", kWeekdays[((days % 7) + 7) % 7], d, kMonths[m - 1],
           static_cast<int>(secs / 3600), static_cast<int>(secs % 3600 / 60));
  return buf;
}

std::string lower(std::string s) {
  for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> jsonLines(const std::string &text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find('\n', start);
    if (nl == std::string::npos) nl = text.size();
    if (nl > start) out.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

}  // namespace

std::string nowText(int64_t local) {
  const int64_t days = floorDiv(local, kDay), secs = local - days * kDay;
  int y, m, d;
  civil(days, y, m, d);
  char buf[64];
  snprintf(buf, sizeof(buf), "%s %d %s %d, %02d:%02d (%s)", kWeekdays[((days % 7) + 7) % 7], d, kMonths[m - 1], y,
           static_cast<int>(secs / 3600), static_cast<int>(secs % 3600 / 60), dayName(days).c_str());
  return buf;
}

std::vector<std::string> recallDays(const std::string &from, const std::string &to, int64_t nowLocal) {
  const int64_t today = floorDiv(nowLocal, kDay);
  int64_t a = 0, b = 0;
  const bool hasFrom = parseDay(from, a), hasTo = parseDay(to, b);
  if (!hasFrom && !hasTo) a = today - (kRecallDays - 1), b = today;
  else if (!hasTo) b = today;
  else if (!hasFrom) a = b - (kRecallDays - 1);
  if (a > b) std::swap(a, b);
  if (b - a + 1 > kRecallMaxSpan) a = b - (kRecallMaxSpan - 1);
  std::vector<std::string> out;
  for (int64_t day = a; day <= b; day++) out.push_back(dayName(day));
  return out;
}

std::vector<std::string> exchanges(const std::string &dayLog, const std::string &name) {
  std::vector<std::string> out;
  for (const std::string &line : jsonLines(dayLog)) {
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
    out.push_back(shortStamp(doc["t"] | static_cast<int64_t>(0)) + " - User: " + (doc["you"] | "") + " / " + name +
                  ": " + (doc["dotty"] | ""));
  }
  return out;
}

std::vector<std::string> byTopic(const std::vector<std::string> &all, const std::string &topic) {
  std::vector<std::string> words;
  std::string word;
  for (const char c : lower(topic) + " ") {
    if (isalnum(static_cast<unsigned char>(c)) || (c & 0x80)) word += c;
    else if (!word.empty()) words.push_back(word), word.clear();
  }
  if (words.empty()) return all;
  std::vector<bool> keep(all.size(), false);
  bool any = false;
  for (size_t i = 0; i < all.size(); i++) {
    const std::string l = lower(all[i]);
    for (const std::string &w : words)
      if (l.find(w) != std::string::npos) {
        any = true;
        keep[i] = true;
        if (i > 0) keep[i - 1] = true;
        if (i + 1 < all.size()) keep[i + 1] = true;
      }
  }
  if (!any) return all;
  std::vector<std::string> out;
  for (size_t i = 0; i < all.size(); i++)
    if (keep[i]) out.push_back(all[i]);
  return out;
}

std::string historyBlock(const std::vector<std::string> &ex) {
  size_t used = 0, first = ex.size();
  while (first > 0 && used + ex[first - 1].size() + 1 <= kRecallMaxChars) used += ex[--first].size() + 1;
  std::string out;
  out.reserve(used);
  for (size_t i = first; i < ex.size(); i++) out += ex[i] + "\n";
  return out;
}

std::vector<std::string> topicWords(const std::string &topic) {
  std::vector<std::string> words;
  std::string word;
  for (const char c : lower(topic) + " ") {
    if (isalnum(static_cast<unsigned char>(c)) || (c & 0x80)) word += c;
    else if (!word.empty()) words.push_back(word), word.clear();
  }
  return words;
}

std::string recallBlock(const std::vector<std::string> &daysNewestFirst,
                        const std::function<std::string(const std::string &day)> &readDay, const std::string &name,
                        const std::string &topic) {
  std::vector<std::string> words = topicWords(topic);
  if (!words.empty()) {  // the topic nowhere in these days: read them all instead
    bool anywhere = false;
    for (size_t i = 0; i < daysNewestFirst.size() && !anywhere; i++) {
      const std::string text = lower(readDay(daysNewestFirst[i]));
      for (const std::string &w : words) anywhere |= text.find(w) != std::string::npos;
    }
    if (!anywhere) words.clear();
  }
  std::vector<std::string> dayBlocks;  // newest first
  size_t used = 0;
  for (const std::string &day : daysNewestFirst) {
    std::vector<std::string> ex = exchanges(readDay(day), name);
    if (!words.empty()) {
      bool here = false;
      for (const std::string &e : ex)
        for (const std::string &w : words) here |= lower(e).find(w) != std::string::npos;
      if (!here) continue;
      ex = byTopic(ex, topic);
    }
    std::string block;
    size_t first = ex.size(), size = 0;  // the newest of this day that still fit
    while (first > 0 && used + size + ex[first - 1].size() + 1 <= kRecallMaxChars) size += ex[--first].size() + 1;
    for (size_t i = first; i < ex.size(); i++) block += ex[i] + "\n";
    used += block.size();
    if (!block.empty()) dayBlocks.push_back(block);
    if (first > 0) break;  // full
  }
  std::string out;
  out.reserve(used);
  for (size_t i = dayBlocks.size(); i-- > 0;) out += dayBlocks[i];
  return out;
}

std::string recallRequest(const std::string &name, const std::string &question, int64_t nowLocal,
                          const std::string &history) {
  JsonDocument doc;
  doc["systemInstruction"]["parts"][0]["text"] =
      "You help " + name + ", a desk companion, remember past conversations with the user. It is now " +
      nowText(nowLocal) + ". Below are past exchanges, each with its day and time (morning is before 12:00, "
      "afternoon 12:00-18:00, evening after 18:00). Answer the question from them only: what was said and roughly "
      "when, in one to four plain sentences, as notes for " + name + " (\"The user said...\"). Keep the user's own "
      "words where they matter. If nothing in them answers the question, reply exactly: Nothing found.";
  doc["contents"][0]["role"] = "user";
  doc["contents"][0]["parts"][0]["text"] = "Question: " + question + "\n\nPast exchanges:\n" + history;
  doc["generationConfig"]["maxOutputTokens"] = 300;
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string recallResult(const Written &found) {
  if (!found.ok) return "You couldn't look back at your past chats just now: tell the user briefly.";
  if (found.text.rfind("Nothing found", 0) == 0)
    return "You found nothing about that in your past chats: say so honestly, in a few words.";
  return "From your past chats: " + found.text + " Tell the user in your own words, briefly.";
}

std::string summaryLine(int64_t localEpoch, const std::string &text) {
  JsonDocument doc;
  doc["t"] = localEpoch;
  doc["text"] = cutField(text, 300);
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string summariesSection(const std::string &jsonl, int max) {
  std::vector<std::string> lines;
  for (const std::string &line : jsonLines(jsonl)) {
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok || !doc["text"].is<const char *>()) continue;
    lines.push_back("- " + shortStamp(doc["t"] | static_cast<int64_t>(0)) + ": " + (doc["text"] | ""));
  }
  if (lines.empty()) return "";
  std::string out = "Your recent chats with the user (oldest first):\n";
  for (size_t i = lines.size() > static_cast<size_t>(max) ? lines.size() - max : 0; i < lines.size(); i++) out += lines[i] + "\n";
  return out;
}
