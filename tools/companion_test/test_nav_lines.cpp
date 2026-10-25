// uses: cartridges/companion/nav_lines.cpp cartridges/companion/nav_route.cpp cartridges/companion/script_writer.cpp cartridges/companion/companion_store.cpp
// Directions: the request that writes every step's line in the companion's voice, the live notes
// (start, re-route, arrival, progress with a question), what the instruction says, nearby answers.
#include <ArduinoJson.h>

#include "check.h"
#include "nav_lines.h"

static bool has(const std::string &s, const std::string &part) {
  return s.find(part) != std::string::npos;
}

static JsonDocument parse(const std::string &s) {
  JsonDocument d;
  deserializeJson(d, s);
  return d;
}

int main() {
  const int64_t now = 1800000000;
  NavRoute r;
  {
    JsonDocument head = parse(R"({"req":1,"route":3,"ok":true,"destination":"Rossio","mode":"walking","distance":1400,"eta":)" +
                              std::to_string(now + 18 * 60) + R"j(,"count":3,"weather":"14 C, cloudy; rain likely within the hour (70%)"})j");
    r.begin(head.as<JsonObjectConst>());
    for (const char *s : {R"({"route":3,"i":0,"text":"","street":"","distance":0,"arrow":"straight"})",
                          R"({"route":3,"i":1,"text":"Turn right onto Rua Augusta","street":"Rua Augusta","distance":300,"arrow":"right","poi":"Cafe Nicola, a cafe, on your left"})",
                          R"({"route":3,"i":2,"text":"Arrive at Rossio","street":"","distance":200,"arrow":"arrive"})"}) {
      JsonDocument d = parse(s);
      r.step(d.as<JsonObjectConst>());
    }
  }
  Profile who = presetProfile("friend");
  who.name = "Percival";
  {
    JsonDocument d = parse(navLinesRequest(who, "- Likes horses", r));
    const std::string sys = d["systemInstruction"]["parts"][0]["text"] | "";
    const std::string text = d["contents"][0]["parts"][0]["text"] | "";
    CHECK("request: one line per step, instruction first, trigger distance, <= 40 words",
          has(sys, "instruction first") && has(text, "40 metres") && has(sys, "40 words") && has(text, "Rua Augusta") &&
              has(sys, "JSON array of 2"));
    CHECK("request: the weather, woven in now and then", has(text, "rain likely within the hour") && has(sys, "weather"));
    CHECK("request: a place passed goes with its step", has(text, "passing: Cafe Nicola, a cafe, on your left"));
    CHECK("request: in character with memories", has(sys, "Percival") && has(sys, "Likes horses"));
    CHECK("request: JSON answer", std::string(d["generationConfig"]["responseMimeType"] | "") == "application/json");
    CHECK("request: varied and spontaneous (the user: always \"my king\", the same things)",
          has(sys, "different") && has(sys, "at most two lines") && (d["generationConfig"]["temperature"] | 0.0) >= 1.0);
    CHECK("request: no example sentences to parrot (it copied the pigeon word for word)",
          !has(sys, "pigeon") && !has(sys, "madmen") && has(sys, "Invent your own"));
    CHECK("request: halfway remarks on long stretches", has(sys, "\"mid\"") && has(sys, "halfway"));
    CHECK("request: places passed, mentioned now and then, never invented", has(sys, "never invent") && has(sys, "now and then"));
  }
  {
    Written ok3{true, R"(["Right onto Rua Augusta, my lord!","Here we are!"])", "", 0};
    Written fenced{true, "```json\n[\"a\",\"b\"]\n```", "", 0};
    Written ok1{true, R"(["only one"])", "", 0};
    Written failed;
    const NavLineSet lines = parseNavLines(ok3, r);
    CHECK("parse: one line per spoken step, empty for the unspoken", lines.turn.size() == 3 && lines.turn[0].empty() &&
                                                                          lines.turn[1] == "Right onto Rua Augusta, my lord!" &&
                                                                          lines.turn[2] == "Here we are!");
    CHECK("parse: code fences tolerated", parseNavLines(fenced, r).turn[2] == "b");
    CHECK("parse: a short array leaves the rest empty", parseNavLines(ok1, r).turn[1] == "only one" && parseNavLines(ok1, r).turn[2].empty());
    CHECK("parse: failed write -> all empty", parseNavLines(failed, r).turn == std::vector<std::string>(3));
    Written objects{true, R"([{"turn":"Right onto Rua Augusta.","mid":"Feeling tired yet?"},{"turn":"Here we are!","mid":"x"}])", "", 0};
    const NavLineSet o = parseNavLines(objects, r);
    // r's step 1 follows a 0 m stretch (not long): its halfway remark is dropped; the arrival's follows 300 m: kept.
    CHECK("parse: turn + mid objects, mid kept only on long stretches", o.turn[1] == "Right onto Rua Augusta." &&
                                                                           o.mid.size() == 3 && o.mid[1].empty() && o.mid[2] == "x");
  }
  {
    const std::string s = navStartNote(r, now);
    CHECK("start note: destination, distance, minutes, first step", has(s, "Rossio") && has(s, "1.4 km") && has(s, "18 minutes") &&
                                                                       has(s, "Turn right onto Rua Augusta") && has(s, "not the user"));
    CHECK("reroute note: off the route, the new first step", has(navRerouteNote(r), "off the route") &&
                                                                 has(navRerouteNote(r), "Turn right onto Rua Augusta"));
    CHECK("arrived note", has(navArrivedNote("Rossio"), "arrived") && has(navArrivedNote("Rossio"), "Rossio"));
    CHECK("arrived at a stop on the way: what's next", has(navArrivedNote("the pharmacy", "the shopping mall"), "the pharmacy") &&
                                                           has(navArrivedNote("the pharmacy", "the shopping mall"), "next") &&
                                                           has(navArrivedNote("the pharmacy", "the shopping mall"), "the shopping mall"));
  }
  {
    NavProgress p;
    p.route = 3;
    p.step = 1;
    p.toTurn = 120;
    p.left = 1200;
    p.eta = now + 14 * 60;
    const std::string trip = navSection({}, &r, &p, now);
    CHECK("section: saved places listed", has(navSection({"Home", "Mum's"}, nullptr, nullptr, now), "Saved places: Home, Mum's"));
    CHECK("section: transit not possible yet", has(navSection({}, nullptr, nullptr, now), "public transport"));
    CHECK("section: a trip in progress", has(trip, "walking to Rossio") && has(trip, "1.2 km") && has(trip, "14 minutes") &&
                                             has(trip, "Turn right onto Rua Augusta in 120 m"));
    CHECK("progress note with a question", has(navProgressNote(r, p, now), "not the user") &&
                                               has(navProgressNote(r, p, now), "in 120 m"));
  }
  {
    std::vector<NearbyPlace> atms;
    JsonDocument a = parse(R"({"req":2,"n":1,"name":"Millennium ATM","category":"ATM","street":"Rua Augusta","distance":150,"minutes":2,"direction":"north of you","phone":"+351 1"})");
    JsonDocument b = parse(R"({"req":2,"n":2,"name":"Caixa ATM","category":"ATM","street":"Rua do Ouro","distance":420,"minutes":5,"direction":"west of you"})");
    NearbyPlace pa, pb;
    CHECK("nearby place parsed", parseNearbyPlace(a.as<JsonObjectConst>(), pa) && pa.n == 1 && pa.name == "Millennium ATM" &&
                                     pa.distance == 150 && pa.minutes == 2 && pa.phone == "+351 1");
    parseNearbyPlace(b.as<JsonObjectConst>(), pb);
    atms = {pa, pb};
    const std::string res = nearbyResult("ATM", atms);
    CHECK("nearby result: names, distance, minutes; never 'open'", has(res, "Millennium ATM") && has(res, "150 m") &&
                                                                     has(res, "2 min") && !has(res, "open"));
    CHECK("nearby result: nothing found", has(nearbyResult("ATM", {}), "Nothing"));
    std::vector<NearbyPlace> hosp = {pa};
    hosp[0].name = "Hospital de Santa Maria";
    hosp[0].category = "Hospital";
    CHECK("nearby: a hospital -> say to call 112 if urgent", has(nearbyResult("hospital", hosp), "112"));
    CHECK("row detail", nearbyRowDetail(pa) == "150 m - 2 min");
    bool missing = false;
    const std::string second = nearbyQuery("nearby 2", atms, missing);
    bool none = false;
    nearbyQuery("nearby 2", {}, none);
    bool plain = true;
    const std::string asIs = nearbyQuery("Rossio", {}, plain);
    CHECK("\"nearby 2\" -> the second's name; no results -> missing; others as they are",
          second == "Caixa ATM" && !missing && none && asIs == "Rossio" && !plain);
  }
  return failures ? 1 : 0;
}
