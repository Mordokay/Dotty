#include "companions.h"

#include <Preferences.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

#include <vector>

#include "history.h"
#include "log.h"
#include "memories.h"
#include "storage.h"

namespace companions {
namespace {

CompanionList list_;
std::vector<Profile> profiles_;
std::string active_;
uint8_t chunk[1024];  // card reads and writes go through internal RAM

String root() {
  return storage::myDataDir() + "/companions";
}

std::string readFile(const String &path) {
  // A power cut between writeFile's remove and rename leaves only the .part: it's the new copy.
  if (!SD_MMC.exists(path) && SD_MMC.exists(path + ".part")) {
    LOGW("companions", "%s missing: using its .part", path.c_str());
    SD_MMC.rename(path + ".part", path);
  }
  File f = SD_MMC.open(path);
  std::string out;
  if (!f) return out;
  out.reserve(f.size());
  for (int n; (n = f.read(chunk, sizeof(chunk))) > 0;) out.append(reinterpret_cast<const char *>(chunk), n);
  f.close();
  return out;
}

bool writeFile(const String &path, const std::string &text) {
  if (text.empty()) return false;
  File f = SD_MMC.open(path + ".part", FILE_WRITE);
  bool ok = static_cast<bool>(f);
  for (size_t at = 0; ok && at < text.size(); at += sizeof(chunk)) {
    const size_t n = min(sizeof(chunk), text.size() - at);
    memcpy(chunk, text.data() + at, n);
    ok = f.write(chunk, n) == n;
  }
  f.close();
  if (ok) {
    SD_MMC.remove(path);
    ok = SD_MMC.rename(path + ".part", path);
  }
  return ok;
}

void migrate() {
  const String data = storage::myDataDir();
  const std::vector<Move> moves = migrationPlan(SD_MMC.exists(data + "/memory"), SD_MMC.exists(data + "/history"),
                                                SD_MMC.exists(data + "/companions/debug/memory"),
                                                SD_MMC.exists(data + "/companions/debug/history"));
  if (!moves.empty()) storage::makeDirs(root() + "/debug");
  for (const Move &m : moves) {
    const bool ok = SD_MMC.rename(data + "/" + m.from.c_str(), data + "/" + m.to.c_str());
    LOGI("companions", "moved %s to %s%s", m.from.c_str(), m.to.c_str(), ok ? "" : " - FAILED");
  }
  LOGI("companions", "migrated: %u moves", static_cast<unsigned>(moves.size()));
}

Profile *mutableFind(const std::string &id) {
  for (Profile &p : profiles_)
    if (p.id == id) return &p;
  return nullptr;
}

void pointStoresAt(const std::string &id) {
  memories::begin(folder(id));
  history::setFolder(folder(id));
}

}  // namespace

String folder(const std::string &id) {
  return root() + "/" + id.c_str();
}

void begin() {
  if (!storage::available()) {
    list_.create("debug");  // the list and the profiles agree (stepping through them divides by the count)
    profiles_ = {presetProfile("debug")};
    active_ = "debug";
    return LOGW("companions", "no card: Debug only, nothing kept");
  }
  migrate();
  storage::makeDirs(root());
  if (!list_.load(readFile(root() + "/index.json"))) {  // first time: the five presets
    for (const char *t : {"coach", "friend", "partner", "teacher", "debug"}) list_.create(t);
    writeFile(root() + "/index.json", list_.save());
  }
  profiles_.clear();
  for (const std::string &id : list_.order()) {
    Profile p;
    if (!parseProfile(readFile(folder(id) + "/profile.json"), p)) {  // a preset's first time (or a lost file)
      p = presetProfile(id);
      if (p.type == "custom") p.id = id;
      p.created = time(nullptr);
      storage::makeDirs(folder(id));
      writeFile(folder(id) + "/profile.json", profileJson(p));
    }
    profiles_.push_back(p);
  }
  Preferences prefs;
  prefs.begin("companion", true);
  active_ = prefs.getString("active", "debug").c_str();
  prefs.end();
  if (!list_.has(active_)) active_ = list_.order().front();
  pointStoresAt(active_);
  LOGI("companions", "%u companions, active %s", static_cast<unsigned>(profiles_.size()), active_.c_str());
}

const Profile &active() {
  static Profile fallback = presetProfile("debug");
  const Profile *p = find(active_);
  return p ? *p : fallback;
}

const CompanionList &list() {
  return list_;
}

const Profile *find(const std::string &id) {
  return mutableFind(id);
}

bool select(const std::string &id) {
  if (!find(id)) return false;
  active_ = id;
  Preferences prefs;
  prefs.begin("companion", false);
  prefs.putString("active", id.c_str());
  prefs.end();
  pointStoresAt(id);
  LOGI("companions", "active: %s", id.c_str());
  return true;
}

bool save(const Profile &p) {
  Profile *mine = mutableFind(p.id);
  if (!mine) return false;
  *mine = p;
  return writeFile(folder(p.id) + "/profile.json", profileJson(p));
}

bool create(const std::string &type, std::string &id) {
  id = list_.create(type);
  if (id.empty()) return false;
  Profile p = presetProfile(type);
  p.id = id;
  p.created = time(nullptr);
  storage::makeDirs(folder(id));
  writeFile(folder(id) + "/profile.json", profileJson(p));
  writeFile(root() + "/index.json", list_.save());
  profiles_.push_back(p);
  return true;
}

bool remove(const std::string &id) {
  std::string next;
  if (!list_.remove(id, next, active_)) return false;
  writeFile(root() + "/index.json", list_.save());
  for (size_t i = 0; i < profiles_.size(); i++)
    if (profiles_[i].id == id) profiles_.erase(profiles_.begin() + i);
  storage::removeTree(folder(id));
  if (next != active_) select(next);
  return true;
}

bool logEvolution(const std::string &id, const std::string &line) {
  File f = SD_MMC.open(folder(id) + "/evolution.jsonl", FILE_APPEND);
  if (!f) return false;
  const bool ok = f.print(line.c_str()) == line.size() && f.print("\n") == 1;
  f.close();
  return ok;
}

std::string evolutionLog(const std::string &id) {
  return readFile(folder(id) + "/evolution.jsonl");
}

}  // namespace companions
