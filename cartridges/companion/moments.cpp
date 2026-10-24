#include "moments.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD_MMC.h>

#include <map>
#include <set>

#include "companions.h"
#include "log.h"
#include "storage.h"

namespace moments {
namespace {

bool available_ = false;
std::map<std::string, MomentList> lists_;
MomentRules rules_;
TimerState timer_;

String listPath(const std::string &companion) {
  return companions::folder(companion) + "/moments.json";
}

String rulesPath() {
  return storage::myDataDir() + "/moment_rules.json";
}

void load(const std::string &companion) {
  MomentList &l = lists_[companion];
  const String path = listPath(companion);
  const std::string json = companions::readText(path);
  if (json.empty()) return;  // none yet
  if (!l.load(json)) {
    LOGW("moments", "%s's moments.json is broken: set aside as moments.bad.json", companion.c_str());
    SD_MMC.remove(companions::folder(companion) + "/moments.bad.json");
    SD_MMC.rename(path, companions::folder(companion) + "/moments.bad.json");
    l = MomentList();
  }
}

// Recordings no moment needs any more: moments that are gone, groups that changed, an old timer's line.
void cleanUp(const std::string &companion) {
  const MomentList &l = lists_[companion];
  std::set<std::string> keep;
  for (const Moment &m : l.all())
    if (m.state == MomentState::Upcoming) keep.insert(std::to_string(m.id));
  for (const std::vector<int> &g : sharedMinutes(l)) keep.insert("group-" + std::to_string(l.find(g[0])->at / 60));
  if (timer_.running && timer_.companion == companion) keep.insert("timer");
  const String dir = companions::folder(companion) + "/moments";
  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) {
    storage::makeDirs(dir);
    return;
  }
  std::vector<String> drop;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String name = f.name();
    f.close();
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    const int dot = name.lastIndexOf('.');
    const String key = dot > 0 ? name.substring(0, dot) : name;
    if (!keep.count(key.c_str())) drop.push_back(name);
  }
  d.close();
  for (const String &name : drop) {
    LOGI("moments", "%s: removed %s (no moment needs it)", companion.c_str(), name.c_str());
    SD_MMC.remove(dir + "/" + name);
  }
}

void loadTimer() {
  Preferences p;
  p.begin("companion", true);
  timer_ = TimerState();
  timer_.running = p.getBool("timerOn", false);
  if (timer_.running) {
    timer_.endsAt = p.getLong64("timerEnd", 0);
    timer_.left = p.getInt("timerLeft", 0);
    timer_.paused = p.getBool("timerPaused", false);
    timer_.tickEvery = p.getInt("timerEvery", 0);
    timer_.label = p.getString("timerLabel", "").c_str();
    timer_.companion = p.getString("timerWho", "").c_str();
    timer_.line = p.getString("timerLine", "").c_str();
    timer_.ready = p.getBool("timerReady", false);
  }
  p.end();
}

}  // namespace

void begin() {
  loadTimer();
  available_ = storage::available();
  if (!available_) {
    LOGW("moments", "no card: moments unavailable (a timer still works)");
    return;
  }
  std::string loadedRules = companions::readText(rulesPath());
  if (!loadedRules.empty() && !rules_.load(loadedRules)) LOGW("moments", "moment_rules.json broken: the defaults");
  size_t count = 0;
  for (const std::string &id : companions::list().order()) {
    load(id);
    cleanUp(id);
    count += lists_[id].all().size();
  }
  LOGI("moments", "%u companions, %u moments, rules ok%s", static_cast<unsigned>(lists_.size()),
       static_cast<unsigned>(count), timer_.running ? ", a timer running" : "");
}

bool available() {
  return available_;
}

std::vector<CompanionMoments> all() {
  std::vector<CompanionMoments> out;
  if (!available_) return out;
  for (const std::string &id : companions::list().order()) out.push_back({id, &lists_[id]});
  return out;
}

MomentList *of(const std::string &companion) {
  if (!available_ || !companions::find(companion)) return nullptr;
  return &lists_[companion];
}

bool save(const std::string &companion) {
  if (!available_ || !lists_.count(companion)) return false;
  const bool ok = companions::writeText(listPath(companion), lists_[companion].save());
  if (!ok) LOGW("moments", "%s: moments NOT saved", companion.c_str());
  return ok;
}

MomentRules &rules() {
  return rules_;
}

bool saveRules() {
  return available_ && companions::writeText(rulesPath(), rules_.save());
}

TimerState &timer() {
  return timer_;
}

void saveTimer() {
  Preferences p;
  p.begin("companion", false);
  p.putBool("timerOn", timer_.running);
  p.putLong64("timerEnd", timer_.endsAt);
  p.putInt("timerLeft", timer_.left);
  p.putBool("timerPaused", timer_.paused);
  p.putInt("timerEvery", timer_.tickEvery);
  p.putString("timerLabel", timer_.label.c_str());
  p.putString("timerWho", timer_.companion.c_str());
  p.putString("timerLine", timer_.line.c_str());
  p.putBool("timerReady", timer_.ready);
  p.end();
}

String recordingPath(const std::string &companion, const std::string &key, const char *ext) {
  return companions::folder(companion) + "/moments/" + key.c_str() + "." + ext;
}

bool recorded(const std::string &companion, const std::string &key, const std::string &signature) {
  if (!available_ || !SD_MMC.exists(recordingPath(companion, key, "wav"))) return false;
  JsonDocument doc;
  JsonDocument filter;
  filter["signature"] = true;
  const std::string json = companions::readText(recordingPath(companion, key, "json"));
  if (deserializeJson(doc, json, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return false;
  return std::string(doc["signature"] | "") == signature;
}

void dropRecording(const std::string &companion, const std::string &key) {
  SD_MMC.remove(recordingPath(companion, key, "wav"));
  SD_MMC.remove(recordingPath(companion, key, "json"));
}

void companionRemoved(const std::string &companion) {
  lists_.erase(companion);
}

}  // namespace moments
