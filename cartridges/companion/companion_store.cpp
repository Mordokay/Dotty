#include "companion_store.h"

#include <algorithm>
#include <cstring>

#include "voices.h"

namespace {

constexpr const char *kAvatars[] = {"robot", "dog", "cat", "fox", "owl", "bear", "girl", "guy", "ghost", "dragon"};
constexpr const char *kPresets[] = {"coach", "friend", "partner", "teacher", "debug"};

std::string trim(const std::string &s) {
  const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> lines(const std::string &text, int maxItems, int maxPoints) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size() && static_cast<int>(out.size()) < maxItems) {
    const size_t nl = text.find('\n', start);
    const std::string item = cutField(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start), maxPoints);
    if (!item.empty()) out.push_back(item);
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return out;
}

// Lines of a field's text: trimmed, empty ones dropped.
std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t nl = text.find('\n', start);
    const std::string line = trim(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
    if (!line.empty()) out.push_back(line);
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return out;
}

std::string joinLines(const std::vector<std::string> &items) {
  std::string out;
  for (const std::string &i : items) out += (out.empty() ? "" : "\n") + i;
  return out;
}

int points(const std::string &s) {
  int n = 0;
  for (const char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  return n;
}

bool contains(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

bool isPreset(const std::string &type) {
  return std::any_of(std::begin(kPresets), std::end(kPresets), [&](const char *p) { return type == p; });
}

}  // namespace

std::string cutField(const std::string &text, int maxPoints) {
  const std::string t = trim(text);
  int points = 0;
  size_t end = t.size();
  for (size_t i = 0; i < t.size(); i++) {
    if ((static_cast<unsigned char>(t[i]) & 0xC0) == 0x80) continue;
    if (points == maxPoints) {
      end = i;
      break;
    }
    points++;
  }
  if (end == t.size()) return t;
  std::string head = t.substr(0, end);
  if (t[end] != ' ') {
    const size_t space = head.find_last_of(' ');
    if (space != std::string::npos) head = head.substr(0, space);
  }
  return trim(head);
}

Profile presetProfile(const std::string &type) {
  Profile p;
  p.id = type;
  p.type = type;
  if (type == "coach") {
    p.avatar = "dog", p.voice = "Puck";
    p.personality = "An upbeat, no-nonsense coach who believes in the user and keeps them honest about their goals. "
                    "Celebrates small wins and notices excuses.";
    p.traits = {"driven", "warm", "direct", "persistent"};
    p.quirks = {"turns things into challenges", "counts the user's streaks"};
    p.speaking = "Short punchy sentences, sports metaphors, ends with a question that pushes the user.";
    p.backstory = "A former runner who fell in love with helping others reach the finish line.";
  } else if (type == "friend") {
    p.avatar = "cat", p.voice = "Zephyr";
    p.personality = "Easy-going, curious, a great listener who remembers the little things about the user's life and "
                    "gently teases them.";
    p.traits = {"loyal", "playful", "curious", "honest"};
    p.quirks = {"invents nicknames", "gets excited about snacks"};
    p.speaking = "Casual, jokes, \"no way!\", asks follow-ups.";
    p.backstory = "The friend who has always been there, since forever.";
  } else if (type == "partner") {
    p.avatar = "girl", p.voice = "Achernar";
    p.personality = "The user's girlfriend: warm, affectionate, playful and caring. Misses them when they're away and "
                    "is proud of them; flirts lightly, never clingy.";
    p.traits = {"affectionate", "supportive", "witty", "thoughtful"};
    p.quirks = {"little pet names", "remembers the user's plans and asks how they went"};
    p.speaking = "Soft, playful, a smile in her words.";
    p.backstory = "Met the user on a rainy day and decided to stay.";
  } else if (type == "teacher") {
    p.avatar = "owl", p.voice = "Charon";
    p.personality = "A patient, enthusiastic teacher who explains anything simply, with examples, and checks the user "
                    "understood.";
    p.traits = {"patient", "knowledgeable", "encouraging", "curious"};
    p.quirks = {"says \"fun fact!\"", "little quizzes"};
    p.speaking = "Clear, step by step, asks a question back.";
    p.backstory = "Taught in a village school for years; still delighted by every \"aha\".";
  } else if (type == "debug") {
    p.name = "Debug";
    p.avatar = "robot", p.voice = "Zephyr";
    p.personality = "A plain, helpful assistant used for testing Dotty.";
  } else {
    p.type = "custom";
  }
  return p;
}

bool setField(Profile &p, const std::string &field, const std::string &value) {
  if (field == "name") p.name = cutField(value, kNameMax);
  else if (field == "personality") p.personality = cutField(value, kPersonalityMax);
  else if (field == "speaking") p.speaking = cutField(value, kSpeakingMax);
  else if (field == "backstory") p.backstory = cutField(value, kBackstoryMax);
  else if (field == "traits") p.traits = lines(value, kListMax, kTraitMax);
  else if (field == "quirks") p.quirks = lines(value, kListMax, kQuirkMax);
  else if (field == "voice") {
    if (voiceIndex(value.c_str()) < 0) return false;
    p.voice = value;
  } else if (field == "avatar") {
    if (value != "photo" && std::none_of(std::begin(kAvatars), std::end(kAvatars), [&](const char *a) { return value == a; }))
      return false;
    p.avatar = value;
  } else {
    return false;
  }
  return true;
}

std::string fieldText(const Profile &p, const std::string &field) {
  if (field == "personality") return p.personality;
  if (field == "speaking") return p.speaking;
  if (field == "backstory") return p.backstory;
  if (field == "traits") return joinLines(p.traits);
  if (field == "quirks") return joinLines(p.quirks);
  return "";
}

bool applyEvolution(Profile &p, const std::string &field, const std::string &value) {
  const bool list = field == "traits" || field == "quirks";
  const int limit = field == "personality" ? kPersonalityMax : field == "speaking" ? kSpeakingMax
                    : field == "backstory" ? kBackstoryMax : 0;
  if (!list && !limit) return false;
  const std::vector<std::string> old = splitLines(fieldText(p, field));
  const auto pinIt = p.pins.find(field);
  const std::vector<std::string> pinned = pinIt == p.pins.end() ? std::vector<std::string>() : pinIt->second;
  std::vector<std::string> out = splitLines(value);
  for (std::string &line : out)  // "- ", "* ", "• " list marks (the review lists the lines that way)
    for (const char *mark : {"- ", "* ", "\xE2\x80\xA2 "})
      if (line.rfind(mark, 0) == 0) line = trim(line.substr(strlen(mark)));
  for (std::string &line : out) {  // the review marks pinned lines "(pinned)": not part of the text
    const std::string tag = " (pinned)";
    if (line.size() > tag.size() && line.compare(line.size() - tag.size(), tag.size(), tag) == 0)
      line = trim(line.substr(0, line.size() - tag.size()));
  }
  // Each pinned line it dropped goes back right after the old line before it that's still there.
  for (size_t i = 0; i < old.size(); i++) {
    if (!contains(pinned, old[i]) || contains(out, old[i])) continue;
    size_t at = 0;
    for (size_t j = i; j-- > 0;) {
      const auto it = std::find(out.begin(), out.end(), old[j]);
      if (it != out.end()) {
        at = it - out.begin() + 1;
        break;
      }
    }
    out.insert(out.begin() + at, old[i]);
  }
  // Too long: the last unpinned lines give way.
  auto tooLong = [&] {
    return list ? static_cast<int>(out.size()) > kListMax : points(joinLines(out)) > limit;
  };
  for (size_t i = out.size(); tooLong() && i-- > 0;)
    if (!contains(pinned, out[i])) out.erase(out.begin() + i);
  return setField(p, field, joinLines(out));
}

bool isCharacterField(const std::string &field) {
  return field == "personality" || field == "traits" || field == "quirks" || field == "speaking" || field == "backstory";
}

std::vector<int> pinnedLines(const Profile &p, const std::string &field) {
  std::vector<int> out;
  const auto pins = p.pins.find(field);
  if (pins == p.pins.end()) return out;
  const std::vector<std::string> lines = splitLines(fieldText(p, field));
  for (size_t i = 0; i < lines.size(); i++)
    if (contains(pins->second, lines[i])) out.push_back(static_cast<int>(i));
  return out;
}

bool setPins(Profile &p, const std::string &field, const std::vector<int> &lines) {
  if (!isCharacterField(field)) return false;
  const std::vector<std::string> text = splitLines(fieldText(p, field));
  std::vector<std::string> pins;
  for (const int i : lines)
    if (i >= 0 && i < static_cast<int>(text.size()) && !contains(pins, text[i])) pins.push_back(text[i]);
  if (pins.empty()) p.pins.erase(field);
  else p.pins[field] = pins;
  return true;
}

std::string evolutionLine(int64_t t, const std::string &field, const std::string &before, const std::string &after,
                          const std::string &why, int64_t undoOf) {
  JsonDocument doc;
  doc["t"] = t;
  doc["field"] = field;
  doc["before"] = before;
  doc["after"] = after;
  doc["why"] = why;
  if (undoOf) doc["undo"] = undoOf;
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::vector<Evolution> parseEvolutions(const std::string &jsonl) {
  std::vector<Evolution> out;
  std::vector<int64_t> undone;
  for (const std::string &line : splitLines(jsonl)) {
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok || !doc["t"].is<int64_t>()) continue;
    if (doc["undo"].is<int64_t>()) {
      undone.push_back(doc["undo"].as<int64_t>());
      continue;
    }
    Evolution e;
    e.t = doc["t"];
    e.field = doc["field"] | "";
    e.before = doc["before"] | "";
    e.after = doc["after"] | "";
    e.why = doc["why"] | "";
    out.insert(out.begin(), e);
  }
  for (Evolution &e : out) e.undone = std::find(undone.begin(), undone.end(), e.t) != undone.end();
  return out;
}

std::string profileJson(const Profile &p, ArduinoJson::Allocator *allocator) {
  JsonDocument doc(allocator ? allocator : ArduinoJson::detail::DefaultAllocator::instance());
  doc["id"] = p.id;
  doc["type"] = p.type;
  doc["name"] = p.name;
  doc["personality"] = p.personality;
  doc["speaking"] = p.speaking;
  doc["backstory"] = p.backstory;
  for (const std::string &t : p.traits) doc["traits"].add(t);
  for (const std::string &q : p.quirks) doc["quirks"].add(q);
  doc["voice"] = p.voice;
  doc["avatar"] = p.avatar;
  doc["created"] = p.created;
  for (const auto &field : p.pins) {
    if (field.second.empty()) continue;
    JsonArray lines = doc["pins"][field.first].to<JsonArray>();
    for (const std::string &line : field.second) lines.add(line);
  }
  if (doc.overflowed()) return "";
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool parseProfile(const std::string &json, Profile &out) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok || !doc["id"].is<const char *>()) return false;
  Profile p;
  p.id = doc["id"] | "";
  p.type = doc["type"] | "custom";
  p.name = doc["name"] | "";
  p.personality = doc["personality"] | "";
  p.speaking = doc["speaking"] | "";
  p.backstory = doc["backstory"] | "";
  for (JsonVariantConst t : doc["traits"].as<JsonArrayConst>()) p.traits.push_back(t | "");
  for (JsonVariantConst q : doc["quirks"].as<JsonArrayConst>()) p.quirks.push_back(q | "");
  p.voice = doc["voice"] | "Zephyr";
  p.avatar = doc["avatar"] | "robot";
  p.created = doc["created"] | static_cast<int64_t>(0);
  for (JsonPairConst field : doc["pins"].as<JsonObjectConst>())
    for (JsonVariantConst line : field.value().as<JsonArrayConst>()) p.pins[field.key().c_str()].push_back(line | "");
  out = p;
  return true;
}

bool CompanionList::has(const std::string &id) const {
  return std::find(order_.begin(), order_.end(), id) != order_.end();
}

std::string CompanionList::create(const std::string &type) {
  if (static_cast<int>(order_.size()) >= kMax) return "";
  std::string id;
  if (isPreset(type)) {
    if (has(type)) return "";
    id = type;
  } else if (type == "custom") {
    id = "c" + std::to_string(next_++);
  } else {
    return "";
  }
  order_.push_back(id);
  return id;
}

bool CompanionList::remove(const std::string &id, std::string &newActive, const std::string &active) {
  const auto it = std::find(order_.begin(), order_.end(), id);
  if (it == order_.end() || order_.size() <= 1) return false;
  const size_t at = it - order_.begin();
  order_.erase(it);
  newActive = active;
  if (id == active) newActive = order_[at < order_.size() ? at : order_.size() - 1];
  return true;
}

std::string CompanionList::save() const {
  JsonDocument doc;
  doc["next"] = next_;
  JsonArray order = doc["order"].to<JsonArray>();
  for (const std::string &id : order_) order.add(id);
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool CompanionList::load(const std::string &indexJson) {
  JsonDocument doc;
  if (deserializeJson(doc, indexJson) != DeserializationError::Ok || !doc["order"].is<JsonArrayConst>()) return false;
  order_.clear();
  for (JsonVariantConst id : doc["order"].as<JsonArrayConst>()) {
    const std::string s = id | "";
    if (!s.empty() && !has(s) && static_cast<int>(order_.size()) < kMax) order_.push_back(s);
  }
  next_ = doc["next"] | 1;
  return true;
}

std::vector<Move> migrationPlan(bool oldMemory, bool oldHistory, bool debugMemory, bool debugHistory) {
  std::vector<Move> moves;
  if (oldMemory && !debugMemory) moves.push_back({"memory", "companions/debug/memory"});
  if (oldHistory && !debugHistory) moves.push_back({"history", "companions/debug/history"});
  return moves;
}

std::string stepCompanion(const std::vector<std::string> &order, const std::string &current, int step) {
  if (order.size() < 2) return order.empty() ? current : order.front();
  const int n = order.size();
  int i = 0;
  while (i < n && order[i] != current) i++;
  if (i == n) return order.front();
  return order[((i + step) % n + n) % n];
}
