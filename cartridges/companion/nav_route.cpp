#include "nav_route.h"

#include <algorithm>

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

namespace {

// A repeatable "random" number in [lo, hi] from a few seeds.
int pick(uint32_t a, uint32_t b, uint32_t c, int lo, int hi) {
  uint32_t x = a * 2654435761u ^ (b + 0x9E3779B9u) * 2246822519u ^ (c + 7u) * 3266489917u;
  x ^= x >> 15;
  x *= 2246822519u;
  x ^= x >> 13;
  return lo + static_cast<int>(x % static_cast<uint32_t>(hi - lo + 1));
}

}  // namespace

int stretchBefore(const NavRoute &r, int i) {
  return i > 0 && i < r.count() ? r.at(i - 1).distance : 0;
}

int triggerFor(const NavRoute &r, int i) {
  const int lo = r.mode == NavMode::Driving ? 250 : r.mode == NavMode::Cycling ? 80 : 30;
  const int hi = r.mode == NavMode::Driving ? 400 : r.mode == NavMode::Cycling ? 150 : 60;
  return pick(static_cast<uint32_t>(r.route()), static_cast<uint32_t>(i), 1, lo, hi);
}

std::vector<int> remarkPoints(const NavRoute &r, int i) {
  std::vector<int> out;
  const int stretch = stretchBefore(r, i);
  if (stretch <= 0 || i >= r.count() || !r.at(i).spoken()) return out;
  const bool drive = r.mode == NavMode::Driving, cycle = r.mode == NavMode::Cycling;
  const int scale = drive ? 12 : cycle ? 3 : 1;
  const int gapLo = drive ? 600 : cycle ? 150 : 50, gapHi = drive ? 900 : cycle ? 250 : 80;
  const int stop = triggerFor(r, i) + 30 * scale;  // never on top of the turn's own line
  int p = stretch - pick(r.route(), i, 2, 20, 40) * scale;  // a little after the previous turn
  for (int k = 0; p > stop && k < 4; k++) {
    out.push_back(p);
    p -= pick(r.route(), i, 10 + k, gapLo, gapHi);
  }
  return out;
}

int triggerMetres(NavMode m) {
  return m == NavMode::Driving ? 300 : m == NavMode::Cycling ? 100 : 40;
}

NavRoute &NavRoute::operator=(const NavRoute &o) {
  if (this == &o) return *this;
  destination = o.destination;
  weather = o.weather;
  error = o.error;
  mode = o.mode;
  distance = o.distance;
  eta = o.eta;
  reroute = o.reroute;
  continues = o.continues;
  choices = o.choices;
  route_ = o.route_;
  req_ = o.req_;
  count_ = o.count_;
  received_ = o.received_;
  steps_.clear();
  steps_.reserve(std::max(kStepsReserved, o.steps_.size()));
  steps_ = o.steps_;
  buffer_.clear();
  buffer_.reserve(kBufferBytes);
  buffer_ = o.buffer_;  // assigning into the reserved capacity keeps it
  if (buffer_.capacity() < kBufferBytes) buffer_.reserve(kBufferBytes);
  return *this;
}

bool NavRoute::begin(JsonObjectConst msg) {
  *this = NavRoute();
  buffer_.reserve(kBufferBytes);  // past the PSRAM threshold: the route's texts never sit in internal RAM
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
  continues = msg["continue"] | false;
  weather = msg["weather"] | "";
  count_ = msg["count"] | 0;
  if (count_ < 0 || count_ > 200) count_ = 0;
  steps_.reserve(std::max(kStepsReserved, static_cast<size_t>(count_)));
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
  const std::string poi = msg["poi"] | "";
  s.poiAt = static_cast<uint32_t>(buffer_.size());
  s.poiLen = static_cast<uint32_t>(poi.size());
  buffer_ += poi;
  s.distance = msg["distance"] | 0;
  parseArrow(msg["arrow"] | "straight", s.arrow);
  s.exit = msg["exit"] | 0;
  if (!s.received) received_++;
  s.received = true;
  return true;
}

void NavLines::set(const std::vector<std::string> &lines) {
  buffer_.clear();
  buffer_.reserve(NavRoute::kBufferBytes);
  at_.clear();
  for (const std::string &l : lines) {
    at_.push_back({static_cast<uint32_t>(buffer_.size()), static_cast<uint32_t>(l.size())});
    buffer_ += l;
  }
}

std::string NavLines::at(size_t i) const {
  return i < at_.size() ? buffer_.substr(at_[i].first, at_[i].second) : std::string();
}

std::string NavRoute::text(int i) const {
  if (i < 0 || i >= count_) return "";
  return buffer_.substr(steps_[i].textAt, steps_[i].textLen);
}

std::string NavRoute::poi(int i) const {
  if (i < 0 || i >= count_) return "";
  return buffer_.substr(steps_[i].poiAt, steps_[i].poiLen);
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
  points_.assign(n, {});
  offset_.assign(n, 0);
  int flat = 0;
  for (int i = 0; i < n; i++) {
    points_[i] = remarkPoints(r, i);
    offset_[i] = flat;
    flat += static_cast<int>(points_[i].size());
  }
  remarkHas_.assign(flat, false);
  remarkReady_.assign(flat, false);
  remarkPlayed_.assign(flat, false);
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

void ClipPlan::setRemark(int i, int k, bool hasLine) {
  if (i >= 0 && i < static_cast<int>(points_.size()) && k >= 0 && k < static_cast<int>(points_[i].size()))
    remarkHas_[offset_[i] + k] = hasLine;
}

int ClipPlan::remarkIndex(int i, int k) const {
  return i >= 0 && i < static_cast<int>(offset_.size()) ? offset_[i] + k : -1;
}

int ClipPlan::remarkCount(int i) const {
  return i >= 0 && i < static_cast<int>(points_.size()) ? static_cast<int>(points_[i].size()) : 0;
}

bool ClipPlan::nextRemarkToRecord(int currentStep, int &i, int &k) const {
  const int n = static_cast<int>(points_.size());
  for (i = currentStep < 0 ? 0 : currentStep; i < n && i < currentStep + 2; i++)
    for (k = 0; k < static_cast<int>(points_[i].size()); k++) {
      const int f = offset_[i] + k;
      if (remarkHas_[f] && !remarkReady_[f] && !remarkPlayed_[f]) return true;
    }
  return false;
}

void ClipPlan::remarkRecorded(int i, int k) {
  const int f = remarkIndex(i, k);
  if (f >= 0 && f < static_cast<int>(remarkReady_.size())) remarkReady_[f] = true;
}

bool ClipPlan::remarkDue(const NavProgress &p, bool busy, const NavRoute &r, int &i, int &k) {
  i = p.step;
  if (i < 0 || i >= static_cast<int>(points_.size()) || busy) return false;
  int due = -1;  // the nearest point already reached; earlier ones passed unplayed are skipped
  for (int j = 0; j < static_cast<int>(points_[i].size()); j++)
    if (p.toTurn <= points_[i][j] && !remarkPlayed_[offset_[i] + j]) due = j;
  if (due < 0 || p.toTurn <= triggerFor(r, i)) return false;
  for (int j = 0; j < due; j++) remarkPlayed_[offset_[i] + j] = true;
  const int f = offset_[i] + due;
  remarkPlayed_[f] = true;
  if (!remarkHas_[f] || !remarkReady_[f]) return false;  // not recorded in time: skipped
  k = due;
  return true;
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
  if (p.toTurn > triggerFor(r, p.step)) return Play::None;
  if (busy) {
    due_ = true;
    return Play::None;
  }
  played_[p.step] = true;
  due_ = false;
  playStep_ = p.step;
  return ready_[p.step] ? Play::Clip : Play::Live;
}
