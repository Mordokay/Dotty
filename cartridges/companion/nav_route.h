#pragma once

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include <ArduinoJson.h>

// Directions on Dotty (spec: docs/superpowers/specs/2026-10-25-companion-directions-design.md). The iPhone
// finds the place, routes and follows you; Dotty holds the trip: the route it was sent (nav.route + a
// nav.step per step, by route number — a re-route is a new number, and anything for an older one is
// ignored), the progress (nav.progress), which step's clip to record next and when a clip plays. Plain C++
// (host-tested).
//
// A route's texts live in one buffer reserved past the PSRAM threshold (4 KB on the companion build),
// never a string per step: internal RAM is a session's limit.

enum class NavMode { Walking, Cycling, Driving };
enum class Arrow { Straight, SlightLeft, Left, SharpLeft, SlightRight, Right, SharpRight, UTurn, Roundabout, Arrive };

bool parseMode(const std::string &name, NavMode &out);
const char *modeName(NavMode m);
bool parseArrow(const std::string &name, Arrow &out);
int triggerMetres(NavMode m);  // a step's clip plays this far before its turn: 40 / 100 / 300

struct NavStep {
  int distance = 0;
  Arrow arrow = Arrow::Straight;
  int exit = 0;  // a roundabout's exit (0 = none)
  uint32_t textAt = 0, textLen = 0, streetAt = 0, streetLen = 0;
  bool received = false;
  bool spoken() const { return textLen > 0; }  // MapKit's first step is empty: no turn there
};

class NavRoute {
 public:
  // A new route (nav.route): false when it isn't one (error or choices, kept for the companion).
  bool begin(JsonObjectConst navRoute);
  bool step(JsonObjectConst navStep);  // false: another route, or out of range
  bool complete() const { return count_ > 0 && received_ == count_; }
  int route() const { return route_; }
  int req() const { return req_; }
  int count() const { return count_; }
  const NavStep &at(int i) const { return steps_[i]; }
  std::string text(int i) const;
  std::string street(int i) const;
  size_t textBytes() const { return buffer_.capacity(); }

  std::string destination, error;
  NavMode mode = NavMode::Walking;
  int distance = 0;
  int64_t eta = 0;
  bool reroute = false;
  std::vector<std::pair<std::string, int>> choices;  // name, metres away

 private:
  int route_ = 0, req_ = 0, count_ = 0, received_ = 0;
  std::vector<NavStep> steps_;
  std::string buffer_;
};

struct NavProgress {
  int route = 0, step = 0, toTurn = 0, left = 0;
  int64_t eta = 0;
};
bool parseProgress(JsonObjectConst msg, int currentRoute, NavProgress &out);  // false for another route

// Which clip to record next (the current step and the next two), and when one plays: once, when the
// metres to its turn reach the trigger distance; waiting while busy (BOOT held, the companion speaking);
// dropped if the step moved on before it could play.
class ClipPlan {
 public:
  enum class Play { None, Clip, Live, Drop };
  void reset(const NavRoute &r);
  int nextToRecord(int currentStep) const;  // -1: nothing to do
  void recorded(int i);
  bool ready(int i) const;
  Play onProgress(const NavProgress &p, NavMode mode, bool busy, const NavRoute &r);
  int playStep() const { return playStep_; }  // the step the last Clip / Live / Drop was for

 private:
  std::vector<bool> spoken_, ready_, played_;
  int step_ = -1, playStep_ = -1;
  bool due_ = false;  // the current step reached its trigger but couldn't play yet
};
