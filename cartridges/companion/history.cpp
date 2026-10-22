#include "history.h"

#include <Preferences.h>
#include <SD_MMC.h>
#include <time.h>

#include <algorithm>

#include "log.h"
#include "shell.h"
#include "storage.h"
#include "usage_rules.h"

namespace {

String dir() {
  return storage::myDataDir() + "/history";
}

int64_t nowUtc() {
  int32_t offset = 0;
  shell::utcOffset(offset);
  return static_cast<int64_t>(time(nullptr)) - offset;
}

}  // namespace

namespace history {

void append(const std::string &you, const std::string &dotty, const std::string &mood, int ms, int tokens) {
  if (!storage::available()) return;
  storage::makeDirs(dir());
  const int64_t local = time(nullptr);
  File f = SD_MMC.open(dir() + "/" + dayFile(local).c_str(), FILE_APPEND);
  if (!f) return;
  f.println(historyLine(local, you, dotty, mood, ms, tokens).c_str());
}

std::vector<String> days() {
  std::vector<String> out;
  File root = SD_MMC.open(dir());
  for (File f = root ? root.openNextFile() : File(); f; f = root.openNextFile()) {
    String name = f.name();
    if (name.endsWith(".jsonl")) out.push_back(name.substring(0, name.length() - 6));
  }
  std::sort(out.begin(), out.end(), [](const String &a, const String &b) { return a > b; });
  return out;
}

String day(const String &yyyymmdd) {
  String out = "[";
  File f = SD_MMC.open(dir() + "/" + yyyymmdd + ".jsonl");
  bool first = true;
  while (f && f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;
    if (!first) out += ",";
    out += line;
    first = false;
  }
  return out + "]";
}

bool forget(const String &yyyymmdd) {
  if (yyyymmdd.length()) return SD_MMC.remove(dir() + "/" + yyyymmdd + ".jsonl");
  for (const String &d : days()) SD_MMC.remove(dir() + "/" + d + ".jsonl");
  return true;
}

}  // namespace history

namespace usage {
namespace {

// NVS companion/usageReset = the Pacific midnight the counters run until.
void rollOver(Preferences &p) {
  const int64_t now = nowUtc();
  if (now >= p.getLong64("usageReset", 0)) {
    p.putLong64("usageReset", nextPacificMidnightUtc(now));
    p.putInt("turns", 0);
    p.putInt("tokens", 0);
  }
}

}  // namespace

void addTurn(int tokens) {
  Preferences p;
  p.begin("companion", false);
  rollOver(p);
  p.putInt("turns", p.getInt("turns", 0) + 1);
  p.putInt("tokens", p.getInt("tokens", 0) + max(0, tokens));
  p.end();
}

void addTokens(int tokens) {
  if (tokens <= 0) return;
  Preferences p;
  p.begin("companion", false);
  rollOver(p);
  p.putInt("tokens", p.getInt("tokens", 0) + tokens);
  p.end();
}

int turnsToday() {
  Preferences p;
  p.begin("companion", false);
  rollOver(p);
  const int n = p.getInt("turns", 0);
  p.end();
  return n;
}

int tokensToday() {
  Preferences p;
  p.begin("companion", false);
  rollOver(p);
  const int n = p.getInt("tokens", 0);
  p.end();
  return n;
}

void markOut(int64_t untilUtc) {
  Preferences p;
  p.begin("companion", false);
  p.putLong64("outUntil", untilUtc);
  p.end();
  LOGI("usage", "out of the free allowance for %lld min", static_cast<long long>((untilUtc - nowUtc()) / 60));
}

int64_t outUntilUtc() {
  Preferences p;
  p.begin("companion", true);
  const int64_t until = p.getLong64("outUntil", 0);
  p.end();
  return until > nowUtc() ? until : 0;
}

}  // namespace usage
