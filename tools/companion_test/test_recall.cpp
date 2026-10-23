// uses: cartridges/companion/recall.cpp cartridges/companion/script_writer.cpp cartridges/companion/companion_store.cpp cartridges/companion/usage_rules.cpp
// Looking back at past chats: by date ("yesterday morning") or topic ("bananas the other day"),
// a text model reads the companion's own history and says what was said; plus a short summary of
// every chat in the instruction.
#include <ArduinoJson.h>

#include "check.h"
#include "recall.h"
#include "usage_rules.h"

static bool has(const std::string &s, const std::string &part) {
  return s.find(part) != std::string::npos;
}

int main() {
  const int64_t now = 1792764300;  // Fri 23 Oct 2026 14:05, local
  CHECK_EQ("now: the date and time for the instruction", nowText(now),
           std::string("Friday 23 October 2026, 14:05 (2026-10-23)"));

  // Which days to read.
  std::vector<std::string> d = recallDays("2026-10-22", "2026-10-22", now);
  CHECK("days: yesterday", d.size() == 1 && d[0] == "2026-10-22");
  d = recallDays("2026-10-21", "", now);
  CHECK("days: from a day until today", d.size() == 3 && d.front() == "2026-10-21" && d.back() == "2026-10-23");
  d = recallDays("", "", now);
  CHECK("days: no dates = the last 30 days", d.size() == 30 && d.front() == "2026-09-24" && d.back() == "2026-10-23");
  d = recallDays("2026-01-01", "2026-10-23", now);
  CHECK("days: a long range keeps its newest 31", d.size() == 31 && d.back() == "2026-10-23");
  d = recallDays("yesterday", "", now);
  CHECK("days: an unreadable date = the last 30 days", d.size() == 30);
  d = recallDays("2026-10-25", "2026-10-20", now);
  CHECK("days: backwards = swapped", d.size() == 6 && d.front() == "2026-10-20");

  // A day's history as readable exchanges.
  const std::string log = historyLine(1792660440, "I love bananas, especially frozen ones", "Frozen bananas are great!", "happy", 900, 10) + "\n" +
                          historyLine(1792660500, "My sister Ana is visiting", "Oh nice!", "happy", 900, 10) + "\n" +
                          historyLine(1792683000, "Work was long", "Sorry to hear", "sad", 900, 10) + "\n";
  const std::vector<std::string> ex = exchanges(log, "Mia");
  CHECK("exchanges: one per line", ex.size() == 3);
  CHECK("exchanges: day, time, both sides", ex.size() == 3 && ex[0] == "Thu 22 Oct 09:14 - User: I love bananas, especially frozen ones / Mia: Frozen bananas are great!");

  // A topic keeps what mentions it, with the exchange before and after for context.
  std::vector<std::string> five = {"a apples", "b nothing", "c bananas here", "d nothing", "e nothing"};
  std::vector<std::string> kept = byTopic(five, "Bananas");
  CHECK("topic: the match and its neighbours", kept.size() == 3 && kept[0] == "b nothing" && kept[2] == "d nothing");
  CHECK("topic: no match = everything", byTopic(five, "kiwi").size() == 5);
  CHECK("topic: none = everything", byTopic(five, "").size() == 5);
  CHECK("topic: several words, any of them", byTopic(five, "apples kiwi").size() == 2);

  std::vector<std::string> many(2000, std::string(100, 'x'));
  many.back() = "newest";
  const std::string capped = historyBlock(many);
  CHECK("block: capped, keeping the newest", capped.size() <= kRecallMaxChars && has(capped, "newest"));

  const std::string body = recallRequest("Mia", "what was I saying yesterday morning", now, historyBlock(ex));
  JsonDocument doc;
  CHECK("request: valid JSON", deserializeJson(doc, body) == DeserializationError::Ok);
  const std::string sys = doc["systemInstruction"]["parts"][0]["text"] | "";
  const std::string user = doc["contents"][0]["parts"][0]["text"] | "";
  CHECK("request: the time now, for 'yesterday'", has(sys, "Friday 23 October 2026, 14:05"));
  CHECK("request: morning means before noon", has(sys, "morning is before 12:00"));
  CHECK("request: only from the chats, else Nothing found", has(sys, "Nothing found"));
  CHECK("request: the question and the chats", has(user, "what was I saying yesterday morning") && has(user, "I love bananas"));

  Written found;
  found.ok = true;
  found.text = "Yesterday at 9:14 the user said they love frozen bananas.";
  CHECK("result: what was found, to tell in its own words", has(recallResult(found), "frozen bananas") && has(recallResult(found), "own words"));
  found.text = "Nothing found.";
  CHECK("result: nothing found = say so honestly", has(recallResult(found), "honestly"));
  CHECK("result: failed = say you can't look back now", has(recallResult(Written()), "couldn't look back"));

  // Summaries: the last ones go into the instruction, oldest first, dated.
  std::string sums;
  for (int i = 0; i < 12; i++) sums += summaryLine(1792660440 + i * 60, "chat " + std::to_string(i)) + "\n";
  const std::string section = summariesSection(sums, 10);
  CHECK("summaries: a heading", has(section, "Your recent chats with the user"));
  CHECK("summaries: the last 10 only", !has(section, "chat 1\n") && has(section, "chat 2") && has(section, "chat 11"));
  CHECK("summaries: dated", has(section, "Thu 22 Oct 09:16: chat 2"));
  CHECK("summaries: none = nothing", summariesSection("", 10).empty());
  CHECK("summaries: cut to 300 characters", summaryLine(1, std::string(500, 'a')).size() < 360);
  return failures ? 1 : 0;
}
