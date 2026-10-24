#include "moment_lines.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "script_writer.h"

namespace {

const char *kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char *kWeekdayNames[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};  // by bit

tm parts(int64_t local) {
  const time_t t = static_cast<time_t>(local);
  tm out;
  gmtime_r(&t, &out);
  return out;
}

int64_t dayStart(int64_t local) {
  return local - ((local % 86400) + 86400) % 86400;
}

std::string hhmm(int64_t local) {
  const tm t = parts(local);
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  return buf;
}

std::string minutesText(int minutes) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", minutes / 60 % 24, minutes % 60);
  return buf;
}

std::string dateText(int64_t local) {
  const tm t = parts(local);
  static const char *kLong[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  static const char *kMonthLong[] = {"January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December"};
  return std::string(kLong[t.tm_wday]) + " " + std::to_string(t.tm_mday) + " " + kMonthLong[t.tm_mon] + " " +
         std::to_string(t.tm_year + 1900);
}

std::string topicKind(const std::string &kind) {
  if (kind == "alarm") return "an alarm (it wakes or alerts them: lively, clear)";
  if (kind == "timer") return "a timer that just ran out";
  if (kind == "spontaneous") return "something you say by yourself, unprompted";
  return "a reminder";
}

int wordCount(const std::string &s) {
  int n = 0;
  bool in = false;
  for (const char c : s) {
    const bool space = c == ' ' || c == '\n' || c == '\t';
    if (!space && !in) n++;
    in = !space;
  }
  return n;
}

// "HH:MM" on that day.
bool parseHhmm(const std::string &s, int64_t day, int64_t &out) {
  int h = -1, m = -1;
  if (std::sscanf(s.c_str(), "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return false;
  out = day + h * 3600 + m * 60;
  return true;
}

std::string upcomingLine(const Moment &m, int64_t now) {
  std::string s = "id " + std::to_string(m.id) + ": " + whenText(m, now) + ", " + kindName(m.kind) + ": " + m.what;
  if (m.style != "spoken") s += " (" + m.style + ")";
  return s;
}

std::vector<const Moment *> usersUpcoming(const MomentList &list) {
  std::vector<const Moment *> out;
  for (const Moment &m : list.all())
    if (m.state == MomentState::Upcoming && m.kind != MomentKind::Spontaneous) out.push_back(&m);
  std::sort(out.begin(), out.end(), [](const Moment *a, const Moment *b) { return a->at < b->at; });
  return out;
}

}  // namespace

std::string styleNote(const std::string &style) {
  if (style == "sung") return "sing it to a simple, cheerful melody";
  if (style == "whispered") return "in a soft whisper";
  if (style == "excited") return "with lots of excitement and energy";
  if (style == "calm") return "in a calm, gentle voice";
  return "";
}

std::string whenText(const Moment &m, int64_t now) {
  const std::string time = hhmm(m.at);
  if (m.repeat == Repeat::Daily) return "Daily " + time;
  if (m.repeat == Repeat::Weekdays) return "Weekdays " + time;
  if (m.repeat == Repeat::Days) {
    std::string days;
    for (int b = 0; b < 7; b++)
      if (m.days & (1 << b)) days += (days.empty() ? "" : ", ") + std::string(kWeekdayNames[b]);
    return days + " " + time;
  }
  const int64_t day = dayStart(m.at), today = dayStart(now);
  if (day == today) return "Today " + time;
  if (day == today + 86400) return "Tomorrow " + time;
  const tm t = parts(m.at);
  return std::string(kDays[t.tm_wday]) + " " + std::to_string(t.tm_mday) + " " + kMonths[t.tm_mon] + " " + time;
}

std::string lineRequest(const Profile &p, const std::string &memories, const std::vector<Topic> &topics, int64_t at,
                        int words) {
  std::string system = writerCharacter(p, memories);
  system += " This line will play by itself at " + hhmm(at) + " on " + dateText(at) +
            ", after a few beeps, when the user may be busy with something else. Write it in the companion's own "
            "voice, in the first person, talking to the user. Spoken English; no headings, lists, markdown, emoji or "
            "stage directions. At most " + std::to_string(words) +
            " words, and as short as the message allows: a plain reminder is usually a sentence or two. Only the "
            "words to say.";
  bool sung = false;
  std::string ask = topics.size() > 1 ? "Say all of these in one go, in a natural order, the most pressing first:\n"
                                      : "Write the line for this:\n";
  for (const Topic &t : topics) {
    ask += "- " + topicKind(t.kind) + ": " + t.what;
    if (!styleNote(t.style).empty()) ask += " (to be delivered " + styleNote(t.style) + ")";
    ask += "\n";
    sung = sung || t.style == "sung";
  }
  if (sung) ask += "Write it as the lyrics of a short, simple song: a verse or two that rhyme.\n";
  JsonDocument doc;
  doc["systemInstruction"]["parts"][0]["text"] = system;
  doc["contents"][0]["role"] = "user";
  doc["contents"][0]["parts"][0]["text"] = ask;
  doc["generationConfig"]["maxOutputTokens"] = 800;
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string readLineNote(const std::string &text, const std::string &style) {
  const std::string how = styleNote(style);
  return "(Dotty, not the user: a line to record for later.) Read the text below aloud exactly, word for word, as "
         "yourself" + (how.empty() ? std::string(", with the feeling it calls for") : ", " + how) +
         ". Don't add, skip or change anything, don't call any tool, and say nothing else.\n\n" + text;
}

std::string recordingInstruction(const std::string &persona) {
  return persona +
         "\n\nRight now you are recording lines to be played later, not chatting: each message gives you a text. "
         "Read it aloud exactly as asked, in your own voice, and say nothing else. Don't call any tool.";
}

std::string planRequest(const Profile &p, const std::string &memories, const std::string &summaries, int64_t day,
                        const MomentRules &rules, int count, const std::vector<int64_t> &taken) {
  std::string system = writerCharacter(p, memories);
  if (!summaries.empty()) system += "\n\n" + summaries + "\n\n";
  system += " You plan the few moments today when the companion will speak up by itself, unprompted, to the user: "
            "a callback to something they said, a question about their plans, a little joke, or simply that it misses "
            "them. Each line in the companion's own voice, first person, spoken English, no emoji, at most 60 words.";
  std::string ask = "Today is " + dateText(day) + ". Plan " + std::to_string(count) + " line" + (count == 1 ? "" : "s") +
                    " between " + minutesText(rules.from) + " and " + minutesText(rules.to) +
                    ", at least 2 hours apart";
  for (const QuietWindow &q : rules.quiet) ask += "; never between " + minutesText(q.from) + " and " + minutesText(q.to);
  for (const int64_t t : taken)
    if (dayStart(t) == day) ask += "; not within 30 minutes of " + hhmm(t);
  ask += ". Answer with JSON only: {\"lines\": [{\"at\": \"HH:MM\", \"what\": \"a few words on what it's about\", "
         "\"line\": \"the words to say\", \"style\": \"spoken|whispered|excited|calm|sung\"}]}";
  JsonDocument doc;
  doc["systemInstruction"]["parts"][0]["text"] = system;
  doc["contents"][0]["role"] = "user";
  doc["contents"][0]["parts"][0]["text"] = ask;
  doc["generationConfig"]["maxOutputTokens"] = 1200;
  doc["generationConfig"]["responseMimeType"] = "application/json";
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::vector<PlannedLine> parsePlan(int httpStatus, const std::string &body, int64_t day, const MomentRules &rules,
                                   const std::vector<int64_t> &taken, int count) {
  std::vector<PlannedLine> out;
  const Written w = parseWritten(httpStatus, body);
  if (!w.ok) return out;
  std::string text = w.text;
  const size_t open = text.find('{'), close = text.rfind('}');  // a ```json fence around it, if any
  if (open == std::string::npos || close == std::string::npos || close < open) return out;
  text = text.substr(open, close - open + 1);
  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) return out;
  for (JsonObjectConst o : doc["lines"].as<JsonArrayConst>()) {
    if (static_cast<int>(out.size()) >= count) break;
    PlannedLine l;
    if (!parseHhmm(o["at"] | "", day, l.at)) continue;
    l.what = o["what"] | "";
    l.line = o["line"] | "";
    l.style = o["style"] | "spoken";
    if (!validStyle(l.style)) l.style = "spoken";
    if (l.line.empty() || wordCount(l.line) > 150) continue;
    if (l.what.empty()) l.what = l.line.substr(0, 60);
    if (!rules.inAllowedHours(l.at) || rules.inQuiet(l.at)) continue;
    bool clash = false;
    for (const PlannedLine &k : out) clash = clash || std::llabs(k.at - l.at) < 2 * 3600;
    for (const int64_t t : taken) clash = clash || std::llabs(t - l.at) < 30 * 60;
    if (!clash) out.push_back(l);
  }
  return out;
}

bool planDue(int64_t now, int64_t lastPlannedDay, const MomentRules &rules) {
  if (rules.perDay <= 0) return false;
  const int64_t today = dayStart(now);
  if (lastPlannedDay >= today) return false;
  return now >= today + (rules.from - 30) * 60LL;
}

std::string momentsSection(const MomentList &list, int64_t now) {
  const std::vector<const Moment *> upcoming = usersUpcoming(list);
  if (upcoming.empty()) return "";
  std::string s = "Moments the user has asked you for (reminders and alarms): " + std::to_string(upcoming.size()) +
                  ". The next ones:";
  for (size_t i = 0; i < upcoming.size() && i < 3; i++) s += "\n- " + upcomingLine(*upcoming[i], now);
  if (upcoming.size() > 3) s += "\nCall list_moments to see them all.";
  else s += "\n(list_moments lists them again.)";
  return s;
}

std::string listMomentsText(const MomentList &list, int64_t now) {
  const std::vector<const Moment *> upcoming = usersUpcoming(list);
  if (upcoming.empty()) return "No reminders or alarms are set.";
  std::string s;
  for (const Moment *m : upcoming) s += (s.empty() ? "" : "\n") + upcomingLine(*m, now);
  return s;
}

std::string firedNote(const std::string &line, const std::string &kind) {
  const std::string what = kind == "alarm" ? "an alarm" : kind == "spontaneous" ? "something you said by yourself"
                                                         : kind == "timer" ? "the end of their timer" : "a reminder";
  return "(Dotty, not the user:) A moment ago you said this to the user, as " + what + " (" + kind + "): \"" + line +
         "\". What they say next may be about it.";
}
