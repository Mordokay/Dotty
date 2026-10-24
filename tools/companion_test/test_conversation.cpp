// uses: cartridges/companion/conversation.cpp
// The open conversation (user, 2026-10-24): it carries on across unlocks until 6 hours of silence or a
// new_conversation call; its older part folds into a summary once the word-for-word part passes
// 2,000 words, like Claude Code's compaction. The history is indexed, not copied a string per turn.
#include <ArduinoJson.h>

#include "check.h"
#include "conversation.h"

static bool has(const std::string &s, const std::string &part) {
  return s.find(part) != std::string::npos;
}

static std::string line(int64_t t, const std::string &you, const std::string &dotty) {
  JsonDocument d;
  d["t"] = t;
  d["you"] = you;
  d["dotty"] = dotty;
  d["mood"] = "happy";
  std::string out;
  serializeJson(d, out);
  return out + "\n";
}

int main() {
  const int64_t h = 3600, base = 1800000000;
  {
    const std::string log = line(base, "Hi", "Hello there!") + "{x\n" + line(base + 60, "How are you?", "Great.");
    const std::vector<TurnRef> t = indexTurns(log);
    CHECK("index: times and words, broken lines skipped",
          t.size() == 2 && t[0].t == base && t[0].words == 3 && t[1].t == base + 60 && t[1].words == 4 &&
              log.substr(t[1].at, t[1].len).find("How are you?") != std::string::npos);
  }
  {
    std::vector<TurnRef> t(4);
    t[0].t = base, t[1].t = base + 2 * h, t[2].t = base + 9 * h, t[3].t = base + 10 * h;
    CHECK_EQ("run start: back to the last 6-hour gap", runStart(t, base + 11 * h), base + 9 * h);
    CHECK_EQ("run start: 6 hours of silence = a new conversation now", runStart(t, base + 16 * h), base + 16 * h);
    CHECK_EQ("run start: nothing yet = now", runStart({}, base), base);
    std::vector<TurnRef> one(2);
    one[0].t = base, one[1].t = base + 5 * h;
    CHECK_EQ("run start: one long run", runStart(one, base + 6 * h), base);
  }
  {
    Conversation c;
    c.start = base + 100;
    c.from = base + 200;
    c.summary = "They talked about Lisbon.";
    Conversation back;
    CHECK("json round trip", parseConversation(conversationJson(c), back) && back.start == c.start && back.from == c.from &&
                                 back.summary == c.summary);
    CHECK("broken json is false", !parseConversation("{x", back));
    CHECK("stored, inside the run: kept", current(c, base).summary == c.summary && current(c, base).from == base + 200);
    const Conversation stale = current(c, base + 300);  // a gap came after it started: a new one
    CHECK("stored, before the run started: a new one", stale.start == base + 300 && stale.from == base + 300 && stale.summary.empty());
    CHECK("nothing stored: the run", current(Conversation(), base + 50).start == base + 50);
  }
  {
    const std::string log = line(base, "Hi there", "Hello you") + line(base + 60, "I like tea", "Me too");
    const std::vector<TurnRef> t = indexTurns(log);
    Conversation c;
    c.start = c.from = base;
    const std::string s = conversationSection(c, log, t, "Percival", base + 60 + 3600);
    CHECK("section: carry on, the turns in order with names",
          has(s, "conversation so far") && has(s, "carry on") && has(s, "User: Hi there") && has(s, "Percival: Hello you") &&
              s.find("Hi there") < s.find("I like tea"));
    CHECK("section: how long ago it paused", has(s, "1 hour ago"));
    Conversation later = c;
    later.from = base + 60;
    const std::string part = conversationSection(later, log, t, "Percival", base + 120);
    CHECK("section: only turns from `from`", has(part, "I like tea") && !has(part, "Hi there"));
    c.summary = "They met.";
    CHECK("section: the summary first", has(conversationSection(c, log, t, "Percival", base + 120), "Earlier, in short: They met."));
    CHECK("section: empty conversation = nothing", conversationSection(Conversation(), "", {}, "P", base).empty());
    std::string many;
    for (int i = 0; i < 400; i++)  // 10 words each: the newest 300 fit 3,000
      many += line(base + i, "turn " + std::to_string(i) + " word word word", "reply reply reply reply reply");
    const std::string capped = conversationSection(c, many, indexTurns(many), "P", base + 500);
    CHECK("section: capped at the newest 3000 words",
          has(capped, "turn 399 ") && has(capped, "turn 100 ") && !has(capped, "turn 99 "));
  }
  {
    std::string log;
    for (int i = 0; i < 50; i++)  // 50 words each
      log += line(base + i * 60, "one two three four five six seven eight nine ten",
                  "a b c d e f g h i j k l m n o p q r s t u v w x y z a b c d e f g h i j k l m n");
    const std::vector<TurnRef> t = indexTurns(log);
    const Fold notYet = foldPlan(t, base + 20 * 60);  // 30 turns = 1,500 words
    const Fold due = foldPlan(t, base);               // 2,500 words
    CHECK("fold: not under 2,000 words", !notYet.due);
    CHECK("fold: past 2,000, the older half", due.due && due.first == 0 && due.end == 25 && due.newFrom == base + 25 * 60);
    const std::string r = foldRequest("Percival", "They met.", log, t, due);
    JsonDocument d;
    deserializeJson(d, r);
    const std::string sys = d["systemInstruction"]["parts"][0]["text"] | "";
    const std::string text = d["contents"][0]["parts"][0]["text"] | "";
    CHECK("fold request: the summary so far and the folded turns only",
          has(text, "They met.") && has(text, "User: one two") && has(text, "Percival: a b c") && has(sys, "200 words") &&
              countWords(text) < 25 * 52 + 40);
  }
  return failures ? 1 : 0;
}
