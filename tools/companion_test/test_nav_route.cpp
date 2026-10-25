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
    for (int d : {45, 38, 42, 35}) {
      const ClipPlan::Play p = plan.onProgress(at(1, d), NavMode::Walking, false, r);
      if (p == ClipPlan::Play::Clip || p == ClipPlan::Play::Live) plays++;
    }
    CHECK_EQ("noise at the trigger plays once", plays, 1);
    CHECK_EQ("the step it played", plan.playStep(), 1);
  }
  {
    ClipPlan fresh;
    fresh.reset(r);
    CHECK("not ready → Live", fresh.onProgress(at(2, 38), NavMode::Walking, false, r) == ClipPlan::Play::Live);
  }
  {
    ClipPlan plan;
    plan.reset(r);
    plan.recorded(2);
    CHECK("busy → None, then Clip when free", plan.onProgress(at(2, 38), NavMode::Walking, true, r) == ClipPlan::Play::None &&
                                                  plan.onProgress(at(2, 35), NavMode::Walking, false, r) == ClipPlan::Play::Clip);
  }
  {
    ClipPlan plan;
    plan.reset(r);
    plan.recorded(3);
    CHECK("a passed step's clip is dropped", plan.onProgress(at(2, 30), NavMode::Walking, true, r) == ClipPlan::Play::None &&
                                                 plan.onProgress(at(3, 300), NavMode::Walking, false, r) == ClipPlan::Play::Drop &&
                                                 plan.onProgress(at(3, 30), NavMode::Walking, false, r) == ClipPlan::Play::Clip);
  }
  {
    ClipPlan plan;
    plan.reset(r);
    plan.noClip(1);  // its line came back empty: said live, the next ones still recorded
    const bool skipped = plan.nextToRecord(1) == 2;
    CHECK("a step without a line is skipped for recording and said live",
          skipped && plan.onProgress(at(1, 30), NavMode::Walking, false, r) == ClipPlan::Play::Live);
  }
  {
    ClipPlan plan;
    plan.reset(r);
    CHECK("an unspoken step never plays", plan.onProgress(at(0, 0), NavMode::Walking, false, r) == ClipPlan::Play::None);
  }
  return failures ? 1 : 0;
}
