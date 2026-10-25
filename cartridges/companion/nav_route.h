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
int longStretch(NavMode m);    // a stretch this long may get a halfway remark: 250 / 600 / 2000

struct NavStep {
  int distance = 0;
  Arrow arrow = Arrow::Straight;
  int exit = 0;  // a roundabout's exit (0 = none)
  uint32_t textAt = 0, textLen = 0, streetAt = 0, streetLen = 0, poiAt = 0, poiLen = 0;
  bool received = false;
  bool spoken() const { return textLen > 0; }  // MapKit's first step is empty: no turn there
};

class NavRoute {
 public:
  static constexpr size_t kBufferBytes = 8192;  // well past the 4 KB PSRAM threshold
  NavRoute() = default;
  // Copies keep the buffer that big: a plain string copy shrinks to its size (internal RAM).
  NavRoute(const NavRoute &o) { *this = o; }
  NavRoute &operator=(const NavRoute &o);
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
  std::string poi(int i) const;  // a place passed at that turn ("Cafe Nicola, a cafe, on your left"), "" for none
  size_t textBytes() const { return buffer_.capacity(); }
  size_t stepBytes() const { return steps_.capacity() * sizeof(NavStep); }
  static constexpr size_t kStepsReserved = 4200 / sizeof(NavStep) + 1;  // the steps array in PSRAM too

  std::string destination, error;
  std::string weather;  // where the trip starts, from the iPhone ("14 C, cloudy; rain likely within the hour (70%)")
  NavMode mode = NavMode::Walking;
  int distance = 0;
  int64_t eta = 0;
  bool reroute = false;
  bool continues = false;  // the next leg after a stop on the way (navigate_to's then)
  std::vector<std::pair<std::string, int>> choices;  // name, metres away

 private:
  int route_ = 0, req_ = 0, count_ = 0, received_ = 0;
  std::vector<NavStep> steps_;
  std::string buffer_;
};

// Each step's line (written by the text model), in one buffer past the PSRAM threshold: a string per
// line would put several KB in internal RAM for the whole trip.
class NavLines {
 public:
  void set(const std::vector<std::string> &lines);
  void clear() { at_.clear(), buffer_.clear(); }
  std::string at(size_t i) const;
  size_t count() const { return at_.size(); }
  size_t bufferBytes() const { return buffer_.capacity(); }

 private:
  std::string buffer_;
  std::vector<std::pair<uint32_t, uint32_t>> at_;  // offset, length
};

// The stretch walked up to step i's turn (MapKit: a step's distance is what comes after its turn).
int stretchBefore(const NavRoute &r, int i);

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
  void noClip(int i);  // no line for it (the writer left it out): said live, not recorded
  bool ready(int i) const;
  Play onProgress(const NavProgress &p, NavMode mode, bool busy, const NavRoute &r);
  int playStep() const { return playStep_; }  // the step the last Clip / Live / Drop was for
  // Halfway remarks on long stretches (the user: "feeling tired yet?"): recorded after the turn clips, played
  // once halfway along the stretch, only from a recorded clip (never live: chatter costs nothing extra).
  void setMid(int i, bool hasLine);
  int nextMidToRecord(int currentStep) const;
  void midRecorded(int i);
  bool midDue(const NavProgress &p, NavMode mode, bool busy, const NavRoute &r);
  int midStep() const { return midStep_; }

 private:
  std::vector<bool> spoken_, ready_, played_, noClip_, hasMid_, midReady_, midPlayed_;
  int step_ = -1, playStep_ = -1, midStep_ = -1;
  bool due_ = false;  // the current step reached its trigger but couldn't play yet
};
