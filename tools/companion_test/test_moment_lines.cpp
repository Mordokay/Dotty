// uses: cartridges/companion/moment_lines.cpp cartridges/companion/moment_store.cpp cartridges/companion/moment_schedule.cpp cartridges/companion/script_writer.cpp cartridges/companion/companion_store.cpp cartridges/companion/persona.cpp
// The words of a moment: what the text model is asked to write (one topic or several at one minute),
// how Live is asked to read it for the recording, the daily plan of the companion's own lines and its
// checks, and what the companion is told about the moments it has set.
#include <ArduinoJson.h>

#include <ctime>

#include "check.h"
#include "moment_lines.h"

static bool has(const std::string &s, const std::string &part) {
  return s.find(part) != std::string::npos;
}

static int64_t local(int y, int mo, int d, int h, int mi) {
  tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  return timegm(&t);
}

// A generateContent answer whose text is the plan's JSON.
static std::string planBody(const std::string &lines) {
  JsonDocument doc;
  doc["candidates"][0]["content"]["parts"][0]["text"] = "{\"lines\": [" + lines + "]}";
  std::string out;
  serializeJson(doc, out);
  return out;
}

static std::string planLine(const char *at, const char *what, const char *style = "spoken") {
  return std::string("{\"at\":\"") + at + "\",\"what\":\"" + what + "\",\"line\":\"Hey, " + what + "!\",\"style\":\"" +
         style + "\"}";
}

int main() {
  Profile p;
  p.name = "Luna";
  p.personality = "Warm and curious.";
  const int64_t day = local(2026, 10, 24, 0, 0);  // Saturday

  CHECK("styleNote: sung asks for a melody, spoken is empty", has(styleNote("sung"), "melody") && styleNote("spoken").empty() &&
                                                                  has(styleNote("whispered"), "whisper"));
  {
    const std::string one = lineRequest(p, "", {{"reminder", "cook dinner", "spoken"}}, day + 18 * 3600, 150);
    CHECK("lineRequest: has every topic's what, the word limit and the companion's name",
          has(one, "cook dinner") && has(one, "150 words") && has(one, "Luna") && has(one, "18:00"));
    const std::string two = lineRequest(p, "", {{"alarm", "leave for the airport", "excited"}, {"reminder", "take the passport", "spoken"}},
                                        day + 7 * 3600, 250);
    CHECK("lineRequest: two topics ask for one line covering both, most pressing first",
          has(two, "leave for the airport") && has(two, "take the passport") && has(two, "most pressing first") &&
              has(two, "250 words"));
    const std::string sung = lineRequest(p, "", {{"reminder", "cook dinner", "sung"}}, day + 18 * 3600, 150);
    CHECK("lineRequest: sung asks for lyrics", has(sung, "song"));
  }
  {
    const std::string note = readLineNote("Time to cook!", "sung");
    CHECK("readLineNote: word for word, with the style", has(note, "word for word") && has(note, "melody") && has(note, "Time to cook!"));
  }
  {
    MomentRules r;  // 10:00-21:00
    const std::vector<PlannedLine> ok =
        parsePlan(200, planBody(planLine("11:00", "your surf trip") + "," + planLine("15:30", "a joke")), day, r, {}, 2);
    CHECK("parsePlan: keeps two good lines", ok.size() == 2 && ok[0].at == day + 11 * 3600 && ok[1].what == "a joke" &&
                                                 ok[0].line == "Hey, your surf trip!" && ok[0].style == "spoken");
    MomentRules q;
    q.quiet.push_back({0x7F, 13 * 60, 14 * 60});
    const std::vector<PlannedLine> dropped = parsePlan(
        200,
        planBody(planLine("08:00", "too early") + "," + planLine("13:30", "in a quiet window") + "," +
                 planLine("11:00", "fine") + "," + planLine("12:00", "an hour later") + "," + planLine("16:10", "near a reminder") + "," +
                 planLine("18:00", "also fine")),
        day, q, {day + 16 * 3600}, 5);
    CHECK("parsePlan: drops lines outside the hours, in a quiet window, < 2 h apart, < 30 min from a taken time",
          dropped.size() == 2 && dropped[0].what == "fine" && dropped[1].what == "also fine");
    const std::vector<PlannedLine> over =
        parsePlan(200, planBody(planLine("11:00", "a") + "," + planLine("14:00", "b") + "," + planLine("17:00", "c")), day, r, {}, 2);
    CHECK("parsePlan: over count keeps the first count", over.size() == 2 && over[1].what == "b");
    const std::vector<PlannedLine> styled = parsePlan(200, planBody(planLine("11:00", "a", "shouted")), day, r, {}, 2);
    CHECK("parsePlan: a bad style becomes spoken", styled.size() == 1 && styled[0].style == "spoken");
    CHECK("parsePlan: HTTP 500 -> none", parsePlan(500, "{\"error\":{\"message\":\"boom\"}}", day, r, {}, 2).empty());
  }
  {
    MomentRules r;
    const int64_t nine29 = day + 9 * 3600 + 29 * 60, nine30 = day + 9 * 3600 + 30 * 60;
    CHECK("planDue: 09:30 yes once, 09:29 no, again the next day",
          !planDue(nine29, 0, r) && planDue(nine30, 0, r) && !planDue(nine30 + 60, day, r) &&
              planDue(nine30 + 86400, day, r));
    MomentRules none;
    none.perDay = 0;
    CHECK("planDue: none a day -> never", !planDue(nine30, 0, none));
  }
  {
    MomentList l;
    CHECK("momentsSection: empty list -> empty", momentsSection(l, day).empty());
    const char *whats[] = {"one", "two", "three", "four", "five"};
    for (int i = 0; i < 5; i++) {
      Moment m;
      m.what = whats[i];
      m.at = day + (10 + i) * 3600;
      l.add(m);
    }
    const std::string s = momentsSection(l, day + 3600);
    CHECK("momentsSection: 5 moments -> count 5 and the next 3",
          has(s, "5") && has(s, "one") && has(s, "three") && !has(s, "four") && has(s, "list_moments"));
    const std::string all = listMomentsText(l, day + 3600);
    CHECK("listMomentsText: ids, times and kinds", has(all, "id 1") && has(all, "id 5") && has(all, "Today 10:00") &&
                                                       has(all, "reminder") && has(all, "five"));
    Moment own;
    own.kind = MomentKind::Spontaneous;
    own.what = "a secret";
    own.at = day + 20 * 3600;
    l.add(own);
    CHECK("listMomentsText: the companion's own lines stay a surprise", !has(listMomentsText(l, day + 3600), "a secret"));
  }
  CHECK("firedNote: reminder", has(firedNote("Time to cook dinner!", "reminder"), "Time to cook dinner!") &&
                                   has(firedNote("x", "reminder"), "reminder"));
  return failures ? 1 : 0;
}
