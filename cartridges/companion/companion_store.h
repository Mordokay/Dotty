#pragma once

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include <ArduinoJson.h>

// The companions (spec: docs/superpowers/specs/2026-10-22-companion-roles-design.md): each one's
// profile, and the list of them. Plain C++ + ArduinoJson, host-tested; companions.* keeps them on
// the SD card.
struct Profile {
  std::string id, type, name;  // type: coach | friend | partner | teacher | debug | custom
  std::string personality, speaking, backstory;
  std::vector<std::string> traits, quirks;
  std::string voice = "Zephyr", avatar = "robot";  // avatar: one of the 10 ids, or "photo"
  int64_t created = 0;
  // Lines that never change by themselves (evolution puts them back), per field: the user's pins.
  std::map<std::string, std::vector<std::string>> pins;
  bool awake() const { return !name.empty(); }  // named = woken up
};

// Limits, in characters (Unicode code points).
constexpr int kNameMax = 24, kPersonalityMax = 600, kSpeakingMax = 300, kBackstoryMax = 600;
constexpr int kListMax = 6, kTraitMax = 40, kQuirkMax = 60;

Profile presetProfile(const std::string &type);  // coach friend partner teacher debug; else custom (empty)
std::string profileJson(const Profile &p, ArduinoJson::Allocator *allocator = nullptr);  // "" when out of memory
bool parseProfile(const std::string &json, Profile &out);
std::string cutField(const std::string &text, int maxPoints);  // trimmed, at a word boundary
// name, personality, speaking, backstory, voice, avatar, traits, quirks (lists = one per line).
// An unknown field, voice or avatar → false and nothing changes.
bool setField(Profile &p, const std::string &field, const std::string &value);

// Evolution (spec: companion roles, "Evolution"): a field as lines (lists one per item, prose
// split on line breaks); "" for anything but the five character fields.
std::string fieldText(const Profile &p, const std::string &field);
// The end-of-chat review's change: the whole new field, one line per item/sentence. Pinned lines
// it dropped go back at their old place; over the limit, the last unpinned lines give way.
// false = not one of personality, traits, quirks, speaking, backstory.
bool applyEvolution(Profile &p, const std::string &field, const std::string &value);
// Pins over BLE go as line numbers of the field as it is now (a pinned sentence can be longer than
// a 512-byte command); the profile keeps their text. setPins replaces that field's pins.
std::vector<int> pinnedLines(const Profile &p, const std::string &field);
bool setPins(Profile &p, const std::string &field, const std::vector<int> &lines);  // false: not a character field
// One line of evolution.jsonl: {t, field, before, after, why[, undo]} (undo = the t it undid).
std::string evolutionLine(int64_t t, const std::string &field, const std::string &before, const std::string &after,
                          const std::string &why, int64_t undoOf = 0);
struct Evolution {
  int64_t t = 0;
  std::string field, before, after, why;
  bool undone = false;
};
std::vector<Evolution> parseEvolutions(const std::string &jsonl);  // newest first, undo lines hidden

class CompanionList {
 public:
  static constexpr int kMax = 8;
  bool load(const std::string &indexJson);  // {"next": N, "order": [ids]}
  std::string save() const;
  const std::vector<std::string> &order() const { return order_; }
  bool has(const std::string &id) const;
  // A preset keeps its type as its id (once); custom ones are c1, c2… (never reused). "" = full,
  // a preset that exists, or an unknown type.
  std::string create(const std::string &type);
  // Never the last one. When `active` goes, newActive = the next in order (the one before if it
  // was last); otherwise newActive = active.
  bool remove(const std::string &id, std::string &newActive, const std::string &active);

 private:
  std::vector<std::string> order_;
  int next_ = 1;
};

// The companion `step` places from `current` in `order` (Dotty's settings row): wraps round;
// `current` itself when there's one companion or none (no card).
std::string stepCompanion(const std::vector<std::string> &order, const std::string &current, int step);

// Moving piece 2a's single memory and history folders into Debug's (paths under data/): move a
// folder only when the old one exists and Debug's doesn't — a second run, or one after a power cut
// mid-move, finishes the job and never overwrites.
struct Move {
  std::string from, to;
};
std::vector<Move> migrationPlan(bool oldMemory, bool oldHistory, bool debugMemory, bool debugHistory);
