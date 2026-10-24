#include "conversation.h"

#include <ArduinoJson.h>

namespace {

// A turn's text, "User: …\nName: …\n", parsed from its line.
std::string turnText(const std::string &log, const TurnRef &r, const std::string &name) {
  JsonDocument doc;
  if (deserializeJson(doc, log.data() + r.at, r.len) != DeserializationError::Ok) return "";
  const std::string you = doc["you"] | "", dotty = doc["dotty"] | "";
  std::string s;
  if (!you.empty()) s += "User: " + you + "\n";
  if (!dotty.empty()) s += name + ": " + dotty + "\n";
  return s;
}

std::string ago(int64_t seconds) {
  if (seconds < 120) return "just now";
  if (seconds < 3600) return std::to_string(seconds / 60) + " minutes ago";
  const int64_t hours = seconds / 3600;
  return std::to_string(hours) + (hours == 1 ? " hour ago" : " hours ago");
}

}  // namespace

std::string conversationJson(const Conversation &c) {
  JsonDocument doc;
  doc["start"] = c.start;
  doc["from"] = c.from;
  doc["summary"] = c.summary;
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool parseConversation(const std::string &json, Conversation &out) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok || !doc.is<JsonObject>()) return false;
  out.start = doc["start"] | static_cast<int64_t>(0);
  out.from = doc["from"] | out.start;
  out.summary = doc["summary"] | "";
  return true;
}

std::vector<TurnRef> indexTurns(const std::string &log) {
  std::vector<TurnRef> out;
  size_t start = 0;
  while (start < log.size()) {
    size_t nl = log.find('\n', start);
    if (nl == std::string::npos) nl = log.size();
    JsonDocument doc;
    if (nl > start && deserializeJson(doc, log.data() + start, nl - start) == DeserializationError::Ok) {
      TurnRef r;
      r.t = doc["t"] | static_cast<int64_t>(0);
      r.at = static_cast<uint32_t>(start);
      r.len = static_cast<uint32_t>(nl - start);
      r.words = countWords(doc["you"] | "") + countWords(doc["dotty"] | "");
      if (r.t && r.words) out.push_back(r);
    }
    start = nl + 1;
  }
  return out;
}

int64_t runStart(const std::vector<TurnRef> &turns, int64_t now) {
  if (turns.empty() || now - turns.back().t >= kConversationGap) return now;
  size_t i = turns.size() - 1;
  while (i > 0 && turns[i].t - turns[i - 1].t < kConversationGap) i--;
  return turns[i].t;
}

Conversation current(const Conversation &stored, int64_t runStarted) {
  if (stored.start && stored.start >= runStarted) return stored;
  Conversation c;
  c.start = c.from = runStarted;
  return c;
}

int countWords(const std::string &text) {
  int n = 0;
  bool in = false;
  for (const char ch : text) {
    const bool space = ch == ' ' || ch == '\n' || ch == '\t';
    if (!space && !in) n++;
    in = !space;
  }
  return n;
}

std::string conversationSection(const Conversation &c, const std::string &log, const std::vector<TurnRef> &turns,
                                const std::string &name, int64_t now) {
  size_t first = turns.size();  // the newest turns from c.from that fit kSectionWords
  int words = 0;
  while (first > 0 && turns[first - 1].t >= c.from && words + turns[first - 1].words <= kSectionWords) {
    words += turns[first - 1].words;
    first--;
  }
  if (first == turns.size() && c.summary.empty()) return "";
  std::string s =
      "The conversation so far: it isn't over, so carry on from it naturally, as if you never stopped (the user may "
      "refer back to anything in it). Don't recap it unless asked.";
  if (first < turns.size()) s += " The last exchange was " + ago(now - turns.back().t) + ".";
  if (!c.summary.empty()) s += "\nEarlier, in short: " + c.summary;
  if (first < turns.size()) {
    s += "\nThe latest exchanges, oldest first:\n";
    for (size_t i = first; i < turns.size(); i++) s += turnText(log, turns[i], name);
    while (!s.empty() && s.back() == '\n') s.pop_back();
  }
  return s;
}

Fold foldPlan(const std::vector<TurnRef> &turns, int64_t from) {
  Fold f;
  size_t first = 0;
  while (first < turns.size() && turns[first].t < from) first++;
  int total = 0;
  for (size_t i = first; i < turns.size(); i++) total += turns[i].words;
  if (total <= kFoldWords || turns.size() - first < 2) return f;
  int words = 0;
  size_t i = first;
  while (i + 1 < turns.size() && words < total / 2) words += turns[i++].words;
  f.due = true;
  f.first = first;
  f.end = i;
  f.newFrom = turns[i].t;
  return f;
}

std::string foldRequest(const std::string &name, const std::string &summary, const std::string &log,
                        const std::vector<TurnRef> &turns, const Fold &fold) {
  JsonDocument doc;
  doc["systemInstruction"]["parts"][0]["text"] =
      "You keep the running summary of an ongoing conversation between the user and " + name +
      ", a small desk companion, so it can carry on later. Write the updated summary of the whole conversation so far "
      "(the summary so far plus the new part), at most 200 words, in plain sentences about \"the user\" and " + name +
      ": what they talked about and in what order, names, facts, plans and decisions, questions still open, the mood, "
      "and anything the user might refer back to. Only the summary.";
  std::string text = "Summary so far: " + (summary.empty() ? std::string("(none yet)") : summary) + "\n\nThe new part:\n";
  for (size_t i = fold.first; i < fold.end && i < turns.size(); i++) text += turnText(log, turns[i], name);
  doc["contents"][0]["role"] = "user";
  doc["contents"][0]["parts"][0]["text"] = text;
  doc["generationConfig"]["maxOutputTokens"] = 400;
  std::string out;
  serializeJson(doc, out);
  return out;
}
