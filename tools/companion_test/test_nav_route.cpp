// uses: cartridges/companion/nav_route.cpp
// Directions on Dotty (spec: docs/superpowers/specs/2026-10-25-companion-directions-design.md): a route
// from the iPhone in pieces (nav.route + nav.step), progress, which clip to record next, and when a clip
// plays, waits or is dropped.
#include <ArduinoJson.h>

#include "check.h"
#include "nav_route.h"

static JsonDocument parse(const std::string &s) {
  JsonDocument d;
  deserializeJson(d, s);
  return d;
}

static JsonDocument routeMsg(int route, int count) {
  return parse(R"({"req":1,"route":)" + std::to_string(route) +
               R"(,"ok":true,"destination":"Rossio","mode":"walking","distance":1400,"eta":1800000000,"count":)" +
               std::to_string(count) + "}");
}

static JsonDocument stepMsg(int route, int i, const std::string &text, const std::string &arrow, int distance = 100) {
  return parse(R"({"route":)" + std::to_string(route) + R"(,"i":)" + std::to_string(i) + R"(,"text":")" + text +
               R"(","street":"Rua )" + std::to_string(i) + R"(","distance":)" + std::to_string(distance) +
               R"(,"arrow":")" + arrow + R"("})");
}

static NavProgress at(int step, int toTurn) {
  NavProgress p;
  p.route = 3;
  p.step = step;
  p.toTurn = toTurn;
  return p;
}

int main() {
  CHECK_EQ("triggers", triggerMetres(NavMode::Walking) * 1000 + triggerMetres(NavMode::Cycling), 40100);
  CHECK_EQ("trigger driving", triggerMetres(NavMode::Driving), 300);
  {
    NavMode m;
    Arrow a;
    CHECK("modes and arrows by name", parseMode("cycling", m) && m == NavMode::Cycling && !parseMode("bus", m) &&
                                          parseArrow("sharp-left", a) && a == Arrow::SharpLeft && parseArrow("roundabout", a) &&
                                          a == Arrow::Roundabout && !parseArrow("diagonal", a) &&
                                          std::string(modeName(NavMode::Driving)) == "driving");
  }
  NavRoute r;
  JsonDocument head = routeMsg(3, 4);
  JsonDocument s0 = stepMsg(3, 0, "", "straight", 0), s1 = stepMsg(3, 1, "Turn right onto Rua Augusta", "right"),
               s2 = stepMsg(3, 2, "Turn left", "left"), s3 = stepMsg(3, 3, "Arrive at Rossio", "arrive");
  const bool began = r.begin(head.as<JsonObjectConst>());
  const bool notYet = !r.complete();
  const bool steps = r.step(s0.as<JsonObjectConst>()) && r.step(s1.as<JsonObjectConst>()) && r.step(s2.as<JsonObjectConst>());
  const bool stillNot = !r.complete();
  r.step(s3.as<JsonObjectConst>());
  CHECK("route assembles from its steps", began && notYet && steps && stillNot && r.complete() && r.count() == 4 &&
                                              r.route() == 3 && r.destination == "Rossio" && r.mode == NavMode::Walking &&
                                              r.distance == 1400);
  {
    NavRoute pr;
    JsonDocument h = routeMsg(7, 2);
    pr.begin(h.as<JsonObjectConst>());
    JsonDocument p0 = parse(R"({"route":7,"i":0,"text":"Head north","street":"","distance":50,"arrow":"straight"})");
    JsonDocument p1 = parse(R"({"route":7,"i":1,"text":"Turn left","street":"Rua Augusta","distance":0,"arrow":"left","poi":"Cafe Nicola, a cafe, on your left"})");
    pr.step(p0.as<JsonObjectConst>());
    pr.step(p1.as<JsonObjectConst>());
    JsonDocument leg = parse(R"({"req":0,"route":8,"ok":true,"destination":"Mall","mode":"walking","distance":900,"eta":1,"count":2,"continue":true})");
    NavRoute second;
    second.begin(leg.as<JsonObjectConst>());
    CHECK("the second leg after a stop (continue)", second.continues && !pr.continues);
    JsonDocument wet = parse(R"j({"req":1,"route":9,"ok":true,"destination":"X","mode":"walking","distance":100,"eta":1,"count":1,"weather":"14 C, cloudy; rain likely within the hour (70%)"})j");
    NavRoute rainy;
    rainy.begin(wet.as<JsonObjectConst>());
    CHECK("the weather where the trip starts", rainy.weather == "14 C, cloudy; rain likely within the hour (70%)" && second.weather.empty());
    CHECK("a place passed at a turn (poi)", pr.poi(1) == "Cafe Nicola, a cafe, on your left" && pr.poi(0).empty());
  }
  CHECK("step text and street", r.text(1) == "Turn right onto Rua Augusta" && r.street(1) == "Rua 1" &&
                                    r.at(1).arrow == Arrow::Right && r.at(1).distance == 100);
  {
    JsonDocument other = stepMsg(2, 1, "old", "left");
    JsonDocument beyond = stepMsg(3, 9, "x", "left");
    CHECK("a step of another route, or past the count, is refused",
          !r.step(other.as<JsonObjectConst>()) && !r.step(beyond.as<JsonObjectConst>()) && r.text(1) == "Turn right onto Rua Augusta");
  }
  {
    JsonDocument p2 = parse(R"({"route":2,"step":1,"toTurn":50,"left":900,"eta":1})");
    JsonDocument p3 = parse(R"({"route":3,"step":1,"toTurn":50,"left":900,"eta":1})");
    NavProgress p;
    CHECK("progress of an old route is ignored", !parseProgress(p2.as<JsonObjectConst>(), 3, p) &&
                                                     parseProgress(p3.as<JsonObjectConst>(), 3, p) && p.toTurn == 50 && p.left == 900);
  }
  CHECK("empty first step: not spoken", !r.at(0).spoken() && r.at(1).spoken());
  CHECK("texts in one buffer, past the PSRAM threshold", r.textBytes() > 4096);
  {
    const NavRoute copy = r;
    NavRoute assigned;
    assigned = r;
    CHECK("a copied route keeps its buffer past the threshold (copies shrink a string to its size)",
          copy.textBytes() > 4096 && assigned.textBytes() > 4096 && copy.text(1) == r.text(1));
  }
  {
    JsonDocument bad = parse(R"({"req":1,"route":4,"ok":false,"error":"no route"})");
    NavRoute f;
    CHECK("a failed route: false, the error kept", !f.begin(bad.as<JsonObjectConst>()) && f.error == "no route" && !f.complete());
    JsonDocument choices = parse(R"({"req":1,"route":5,"ok":false,"choices":[{"n":1,"name":"Pharmacy A","distance":200},{"n":2,"name":"Pharmacy B","distance":900}]})");
    NavRoute c;
    c.begin(choices.as<JsonObjectConst>());
    CHECK("choices kept", c.choices.size() == 2 && c.choices[1].first == "Pharmacy B" && c.choices[1].second == 900);
  }
  {
    ClipPlan plan;
    plan.reset(r);
    const int first = plan.nextToRecord(0);  // step 0 is never spoken
    plan.recorded(1);
    plan.recorded(2);
    plan.recorded(3);
    CHECK("record ahead: the first missing within 3, unspoken skipped", first == 1 && plan.nextToRecord(1) == -1 && plan.ready(2));
  }
  {
    ClipPlan plan;
    plan.reset(r);
    plan.recorded(1);
    int plays = 0;
    const int t1 = triggerFor(r, 1);  // the step's own distance (random 30-60 m walking)
    for (int d : {t1 + 5, t1 - 2, t1 + 2, t1 - 5}) {
      const ClipPlan::Play p = plan.onProgress(at(1, d), NavMode::Walking, false, r);
      if (p == ClipPlan::Play::Clip || p == ClipPlan::Play::Live) plays++;
    }
    CHECK_EQ("noise at the trigger plays once", plays, 1);
    CHECK_EQ("the step it played", plan.playStep(), 1);
  }
  {
    ClipPlan fresh;
    fresh.reset(r);
    CHECK("not ready → Live", fresh.onProgress(at(2, 25), NavMode::Walking, false, r) == ClipPlan::Play::Live);
  }
  {
    ClipPlan plan;
    plan.reset(r);
    plan.recorded(2);
    CHECK("busy → None, then Clip when free", plan.onProgress(at(2, 25), NavMode::Walking, true, r) == ClipPlan::Play::None &&
                                                  plan.onProgress(at(2, 24), NavMode::Walking, false, r) == ClipPlan::Play::Clip);
  }
  {
    ClipPlan plan;
    plan.reset(r);
    plan.recorded(3);
    CHECK("a passed step's clip is dropped", plan.onProgress(at(2, 25), NavMode::Walking, true, r) == ClipPlan::Play::None &&
                                                 plan.onProgress(at(3, 300), NavMode::Walking, false, r) == ClipPlan::Play::Drop &&
                                                 plan.onProgress(at(3, 25), NavMode::Walking, false, r) == ClipPlan::Play::Clip);
  }
  {
    ClipPlan plan;
    plan.reset(r);
    plan.noClip(1);  // its line came back empty: said live, the next ones still recorded
    const bool skipped = plan.nextToRecord(1) == 2;
    CHECK("a step without a line is skipped for recording and said live",
          skipped && plan.onProgress(at(1, 25), NavMode::Walking, false, r) == ClipPlan::Play::Live);
  }
  CHECK("the steps array past the PSRAM threshold too", r.stepBytes() > 4096);
  {
    NavLines lines;
    lines.set({"", "Right onto Rua Augusta, my lord!", "", "Here we are."});
    CHECK("lines: one buffer past the threshold, by step", lines.bufferBytes() > 4096 && lines.count() == 4 &&
                                                               lines.at(1) == "Right onto Rua Augusta, my lord!" &&
                                                               lines.at(0).empty() && lines.at(3) == "Here we are." &&
                                                               lines.at(9).empty());
  }
  {
    ClipPlan plan;
    plan.reset(r);
    CHECK("an unspoken step never plays", plan.onProgress(at(0, 0), NavMode::Walking, false, r) == ClipPlan::Play::None);
  }
  {  // Remarks along long stretches and a turn distance that isn't always 40 m (the user, 2026-10-25)
    NavRoute w;
    JsonDocument h = routeMsg(5, 4);
    w.begin(h.as<JsonObjectConst>());
    JsonDocument a0 = stepMsg(5, 0, "Head north", "straight", 363);  // the 363 m stretch up to step 1's turn
    JsonDocument a1 = stepMsg(5, 1, "Turn left", "left", 120);
    JsonDocument a2 = stepMsg(5, 2, "Turn right", "right", 70);
    JsonDocument a3 = stepMsg(5, 3, "Arrive", "arrive", 0);
    for (JsonDocument *d : {&a0, &a1, &a2, &a3}) w.step(d->as<JsonObjectConst>());
    bool inRange = true, varies = false;
    for (int i = 0; i < 4; i++) {
      const int t = triggerFor(w, i);
      inRange = inRange && t >= 30 && t <= 60 && t == triggerFor(w, i);  // walking 30-60 m, the same every time asked
      varies = varies || t != triggerFor(w, 0);
    }
    CHECK("turn distance: 30-60 m walking, random per step, repeatable", inRange && varies);
    CHECK("stretch before a turn", stretchBefore(w, 1) == 363 && stretchBefore(w, 2) == 120 && stretchBefore(w, 0) == 0);
    const std::vector<int> p1 = remarkPoints(w, 1), p2 = remarkPoints(w, 2), p3 = remarkPoints(w, 3);
    bool ordered = p1.size() == 4;
    for (size_t k = 0; k < p1.size(); k++)
      ordered = ordered && p1[k] < 363 && p1[k] > triggerFor(w, 1) + 25 && (k == 0 || p1[k] < p1[k - 1] - 45);
    CHECK("remarks: 363 m gives 4 (every 50-80 m), before the turn's own line", ordered);
    CHECK("remarks: 120 m gives 1, 70 m none", p2.size() == 1 && p3.empty());
    auto at5 = [](int step, int toTurn) {
      NavProgress p;
      p.route = 5;
      p.step = step;
      p.toTurn = toTurn;
      return p;
    };
    ClipPlan plan;
    plan.reset(w);
    for (int k = 0; k < 4; k++) plan.setRemark(1, k, true);
    plan.setRemark(2, 0, true);
    int ri = -1, rk = -1;
    const bool first = plan.nextRemarkToRecord(1, ri, rk) && ri == 1 && rk == 0;
    int di = -1, dk = -1;
    ClipPlan fresh = plan;  // not recorded when its point comes: skipped for good (never said live)
    const bool notReady = !fresh.remarkDue(at5(1, p1[0] - 1), false, w, di, dk);
    for (int k = 0; k < 4; k++) plan.remarkRecorded(1, k);
    const bool early = !plan.remarkDue(at5(1, p1[0] + 5), false, w, di, dk);
    const bool busy = !plan.remarkDue(at5(1, p1[0] - 1), true, w, di, dk);
    const bool now = plan.remarkDue(at5(1, p1[0] - 2), false, w, di, dk) && di == 1 && dk == 0;
    const bool once = !plan.remarkDue(at5(1, p1[0] - 3), false, w, di, dk);
    // Two points passed while busy: only the latest plays, the earlier is skipped.
    const bool skip = plan.remarkDue(at5(1, p1[2] - 1), false, w, di, dk) && dk == 2 && !plan.remarkDue(at5(1, p1[2] - 2), false, w, di, dk);
    CHECK("remarks: recorded in order, each once at its point, not while busy, a missed one skipped",
          first && notReady && early && busy && now && once && skip);
    int ni = -1, nk = -1;
    plan.remarkRecorded(2, 0);
    CHECK("remarks: none left to record", !plan.nextRemarkToRecord(1, ni, nk));
    CHECK("remark index: flattened by step, then remark", plan.remarkIndex(1, 3) == 3 && plan.remarkIndex(2, 0) == 4);
  }
  return failures ? 1 : 0;
}
