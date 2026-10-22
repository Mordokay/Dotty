#include "memory_store.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <iterator>

namespace {

constexpr const char *kGroups[] = {"you", "people", "likes", "plans", "other"};
constexpr const char *kLabels[] = {"About you", "People", "Likes & dislikes", "Plans & dates", "Other"};

std::string trim(const std::string &s) {
  const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Lower-case ASCII, one space between words, no quotes around it or final full stop: two memories
// that differ only so are the same (Gemini sees each memory ending with "." and may say it back so).
std::string key(const std::string &text) {
  std::string t = trim(text);
  while (!t.empty() && (t.back() == '.' || t.back() == '!' || t.back() == '?' || t.back() == '"' || t.back() == '\''))
    t.pop_back();
  while (!t.empty() && (t.front() == '"' || t.front() == '\'')) t.erase(0, 1);
  std::string out;
  bool space = false;
  for (unsigned char c : trim(t)) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
  }
  return out;
}

}  // namespace

std::string MemoryStore::cut(const std::string &text) {
  const std::string t = trim(text);
  size_t points = 0, end = 0;
  for (size_t i = 0; i < t.size(); i++) {
    if ((static_cast<unsigned char>(t[i]) & 0xC0) == 0x80) continue;  // inside a character
    if (points == static_cast<size_t>(kMaxText)) {
      end = i;
      break;
    }
    points++;
  }
  if (points < static_cast<size_t>(kMaxText) || end == 0) return t;  // fits
  // Over the limit: back to the last space before the cut, so no word is broken.
  std::string head = t.substr(0, end);
  if (t[end] != ' ') {
    const size_t space = head.find_last_of(' ');
    if (space != std::string::npos) head = head.substr(0, space);
  }
  return trim(head);
}

bool MemoryStore::validGroup(const std::string &group) {
  return std::any_of(std::begin(kGroups), std::end(kGroups), [&](const char *g) { return group == g; });
}

const char *MemoryStore::label(const std::string &group) {
  for (size_t i = 0; i < std::size(kGroups); i++)
    if (group == kGroups[i]) return kLabels[i];
  return "Other";
}

void MemoryStore::clear() {
  memories_.clear();
}

const Memory *MemoryStore::find(int id) const {
  for (const Memory &m : memories_)
    if (m.id == id) return &m;
  return nullptr;
}

MemoryStore::Added MemoryStore::add(const std::string &text, const std::string &group, const std::string &by,
                                    int64_t now) {
  Added r;
  const std::string t = cut(text);
  if (!validGroup(group)) {
    r.error = "group";
    return r;
  }
  if (t.empty()) {
    r.error = "empty";
    return r;
  }
  const std::string k = key(t);
  for (const Memory &m : memories_) {
    if (key(m.text) == k) {
      r.ok = true;
      r.id = m.id;
      return r;
    }
  }
  if (static_cast<int>(memories_.size()) >= kMax) {
    r.error = "full";
    return r;
  }
  Memory m;
  m.id = next_++;
  m.group = group;
  m.text = t;
  m.by = by;
  m.added = m.changed = now;
  memories_.push_back(m);
  r.ok = true;
  r.id = m.id;
  return r;
}

bool MemoryStore::update(int id, const std::string *text, const std::string *group, int64_t now) {
  Memory *m = nullptr;
  for (Memory &x : memories_)
    if (x.id == id) m = &x;
  if (!m) return false;
  const std::string t = text ? cut(*text) : m->text;
  if (t.empty() || (group && !validGroup(*group))) return false;
  m->text = t;
  if (group) m->group = *group;
  m->changed = now;
  return true;
}

bool MemoryStore::remove(int id) {
  const auto it = std::find_if(memories_.begin(), memories_.end(), [&](const Memory &m) { return m.id == id; });
  if (it == memories_.end()) return false;
  memories_.erase(it);
  return true;
}

std::vector<Memory> MemoryStore::page(int offset, int limit) const {
  std::vector<Memory> out;
  for (int i = static_cast<int>(memories_.size()) - 1 - offset; i >= 0 && static_cast<int>(out.size()) < limit; i--)
    out.push_back(memories_[i]);
  return out;
}

std::string MemoryStore::promptSection() const {
  if (memories_.empty()) return "";
  std::string s =
      "What you remember about the user (use it naturally, don't recite it; numbers are for the memory tools):";
  if (static_cast<int>(memories_.size()) >= kMax)
    s += "\nMemory is full (300): ask the user to delete some in the app before remembering more.";
  for (size_t g = 0; g < std::size(kGroups); g++) {
    std::string line;
    for (const Memory &m : memories_) {
      if (m.group != kGroups[g]) continue;
      line += " [" + std::to_string(m.id) + "] " + m.text;
      const char last = m.text.empty() ? '.' : m.text.back();
      if (last != '.' && last != '!' && last != '?') line += ".";
    }
    if (!line.empty()) s += "\n" + std::string(kLabels[g]) + ":" + line;
  }
  return s;
}

std::string MemoryStore::save(ArduinoJson::Allocator *allocator) const {
  JsonDocument doc(allocator ? allocator : ArduinoJson::detail::DefaultAllocator::instance());
  doc["next"] = next_;
  JsonArray list = doc["memories"].to<JsonArray>();
  for (const Memory &m : memories_) {
    JsonObject o = list.add<JsonObject>();
    o["id"] = m.id;
    o["group"] = m.group;
    o["text"] = m.text;
    o["by"] = m.by;
    o["added"] = m.added;
    o["changed"] = m.changed;
  }
  if (doc.overflowed()) return "";
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool MemoryStore::load(const std::string &json) {
  memories_.clear();
  next_ = 1;
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok || !doc["memories"].is<JsonArrayConst>()) return false;
  for (JsonObjectConst o : doc["memories"].as<JsonArrayConst>()) {
    Memory m;
    m.id = o["id"] | 0;
    m.group = o["group"] | "";
    m.text = o["text"] | "";
    m.by = o["by"] | "dotty";
    m.added = o["added"] | static_cast<int64_t>(0);
    m.changed = o["changed"] | m.added;
    if (m.id <= 0 || !validGroup(m.group) || m.text.empty() || find(m.id)) continue;  // skip a bad entry
    memories_.push_back(m);
    next_ = std::max(next_, m.id + 1);
  }
  next_ = std::max(next_, doc["next"] | 1);
  return true;
}
