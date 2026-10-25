#include "nav_route.h"

namespace {

const char *const kModes[] = {"walking", "cycling", "driving"};
const char *const kArrows[] = {"straight",     "slight-left", "left",  "sharp-left", "slight-right",
                               "right",        "sharp-right", "uturn", "roundabout", "arrive"};

}  // namespace

bool parseMode(const std::string &name, NavMode &out) {
  for (int i = 0; i < 3; i++)
    if (name == kModes[i]) {
      out = static_cast<NavMode>(i);
      return true;
    }
  return false;
}

const char *modeName(NavMode m) {
  return kModes[static_cast<int>(m)];
}

bool parseArrow(const std::string &name, Arrow &out) {
  for (int i = 0; i < 10; i++)
    if (name == kArrows[i]) {
      out = static_cast<Arrow>(i);
      return true;
    }
  return false;
}

int triggerMetres(NavMode m) {
  return m == NavMode::Driving ? 300 : m == NavMode::Cycling ? 100 : 40;
}

bool NavRoute::begin(JsonObjectConst msg) {
  *this = NavRoute();
  buffer_.reserve(4096);  // past the PSRAM threshold: the route's texts never sit in internal RAM
  req_ = msg["req"] | 0;
  route_ = msg["route"] | 0;
  error = msg["error"] | "";
  for (JsonObjectConst c : msg["choices"].as<JsonArrayConst>())
    choices.push_back({c["name"] | "", c["distance"] | 0});
  if (!(msg["ok"] | false)) return false;
  destination = msg["destination"] | "";
  parseMode(msg["mode"] | "walking", mode);
  distance = msg["distance"] | 0;
  eta = msg["eta"] | static_cast<int64_t>(0);
  reroute = msg["reroute"] | false;
  count_ = msg["count"] | 0;
  if (count_ < 0 || count_ > 200) count_ = 0;
  steps_.assign(count_, NavStep());
  return count_ > 0;
}

bool NavRoute::step(JsonObjectConst msg) {
  const int i = msg["i"] | -1;
  if ((msg["route"] | 0) != route_ || i < 0 || i >= count_) return false;
  NavStep &s = steps_[i];
  const std::string text = msg["text"] | "", street = msg["street"] | "";
  s.textAt = static_cast<uint32_t>(buffer_.size());
  s.textLen = static_cast<uint32_t>(text.size());
  buffer_ += text;
  s.streetAt = static_cast<uint32_t>(buffer_.size());
  s.streetLen = static_cast<uint32_t>(street.size());
  buffer_ += street;
  s.distance = msg["distance"] | 0;
  parseArrow(msg["arrow"] | "straight", s.arrow);
  s.exit = msg["exit"] | 0;
  if (!s.received) received_++;
  s.received = true;
  return true;
}

std::string NavRoute::text(int i) const {
  if (i < 0 || i >= count_) return "";
  return buffer_.substr(steps_[i].textAt, steps_[i].textLen);
}

std::string NavRoute::street(int i) const {
  if (i < 0 || i >= count_) return "";
  return buffer_.substr(steps_[i].streetAt, steps_[i].streetLen);
}

bool parseProgress(JsonObjectConst msg, int currentRoute, NavProgress &out) {
  if ((msg["route"] | 0) != currentRoute) return false;
  out.route = currentRoute;
  out.step = msg["step"] | 0;
  out.toTurn = msg["toTurn"] | 0;
  out.left = msg["left"] | 0;
  out.eta = msg["eta"] | static_cast<int64_t>(0);
  return true;
}

void ClipPlan::reset(const NavRoute &r) {
  const int n = r.count();
  spoken_.assign(n, false);
  ready_.assign(n, false);
  played_.assign(n, false);
  noClip_.assign(n, false);
  for (int i = 0; i < n; i++) spoken_[i] = r.at(i).spoken();
  step_ = playStep_ = -1;
  due_ = false;
}

int ClipPlan::nextToRecord(int currentStep) const {
  const int n = static_cast<int>(spoken_.size());
  int seen = 0;
  for (int i = currentStep < 0 ? 0 : currentStep; i < n && seen < 3; i++) {
    if (!spoken_[i]) continue;
    seen++;
    if (!ready_[i] && !played_[i] && !noClip_[i]) return i;
  }
  return -1;
}

void ClipPlan::recorded(int i) {
  if (i >= 0 && i < static_cast<int>(ready_.size())) ready_[i] = true;
}

void ClipPlan::noClip(int i) {
  if (i >= 0 && i < static_cast<int>(noClip_.size())) noClip_[i] = true;
}

bool ClipPlan::ready(int i) const {
  return i >= 0 && i < static_cast<int>(ready_.size()) && ready_[i];
}

ClipPlan::Play ClipPlan::onProgress(const NavProgress &p, NavMode mode, bool busy, const NavRoute &r) {
  const int n = static_cast<int>(spoken_.size());
  if (p.step != step_) {
    const int was = step_;
    const bool missed = due_ && was >= 0 && was < n && !played_[was];
    step_ = p.step;
    due_ = false;
    if (missed) {  // its turn has passed: too late for that line
      played_[was] = true;
      playStep_ = was;
      return Play::Drop;
    }
  }
  if (p.step < 0 || p.step >= n || !spoken_[p.step] || played_[p.step]) return Play::None;
  if (p.toTurn > triggerMetres(mode)) return Play::None;
  if (busy) {
    due_ = true;
    return Play::None;
  }
  played_[p.step] = true;
  due_ = false;
  playStep_ = p.step;
  return ready_[p.step] ? Play::Clip : Play::Live;
}
