#pragma once

#include <stdint.h>

#include <ArduinoJson.h>

#include <string>
#include <vector>

// What the companion remembers about the user (spec:
// docs/superpowers/specs/2026-10-22-companion-memories-design.md). Plain C++ + ArduinoJson,
// host-tested; memories.* keeps it on the SD card.
struct Memory {
  int id = 0;
  std::string group, text, by;  // group: you | people | likes | plans | other; by: dotty | app
  int64_t added = 0, changed = 0;  // local epoch seconds
};

class MemoryStore {
 public:
  static constexpr int kMax = 300;      // memories
  static constexpr int kMaxText = 200;  // characters (Unicode code points)

  struct Added {
    bool ok = false;
    int id = 0;
    std::string error;  // "full" | "empty" | "group"
  };

  // {"next": N, "memories": [{id, group, text, by, added, changed}]}. A broken file → false and
  // an empty store.
  bool load(const std::string &json);
  // "" when the document couldn't be built (out of memory): never a file with memories missing.
  // `allocator`: where ArduinoJson builds it (PSRAM on Dotty: a save happens mid-session).
  std::string save(ArduinoJson::Allocator *allocator = nullptr) const;

  // The same text as an existing memory (case and spaces aside) adds nothing: ok, that id.
  Added add(const std::string &text, const std::string &group, const std::string &by, int64_t now);
  // nullptr leaves a field as it is; an unknown id, an empty text or a bad group → false.
  bool update(int id, const std::string *text, const std::string *group, int64_t now);
  bool remove(int id);
  void clear();

  const std::vector<Memory> &all() const { return memories_; }  // oldest first
  const Memory *find(int id) const;
  std::vector<Memory> page(int offset, int limit) const;  // newest first
  // For the system instruction: grouped, numbered; "" when there's nothing to remember.
  std::string promptSection() const;

  static std::string cut(const std::string &text);  // trimmed, ≤ kMaxText, at a word boundary
  static bool validGroup(const std::string &group);
  static const char *label(const std::string &group);  // "Likes & dislikes"…

 private:
  std::vector<Memory> memories_;
  int next_ = 1;
};
