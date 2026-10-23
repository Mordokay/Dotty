#pragma once

#include <string>
#include <vector>

#include "companion_store.h"

// Macros (the user, 2026-10-23): when the user says one of a macro's phrases, the companion calls
// run_macro and Dotty runs the macro's prompt through the text model (~150 words, written in the
// active companion's voice) and has Live read it aloud — the writer's path (script_writer.h).
// Shared by all companions, kept as data/macros.json. Plain C++ (host-tested).
struct Macro {
  int id = 0;
  std::string name;
  std::string triggers;  // phrases, one per line
  std::string prompt;    // what the text model is asked to write
};

class MacroList {
 public:
  static constexpr int kMax = 12, kNameMax = 40, kTriggersMax = 300, kPromptMax = 2000;
  static MacroList withPresets();  // "Talk dirty to me"

  bool load(const std::string &json);  // {"next": N, "macros": [{id, name, triggers, prompt}]}
  std::string save() const;
  const std::vector<Macro> &all() const { return macros_; }
  const Macro *find(int id) const;
  int add(const std::string &name, const std::string &triggers, const std::string &prompt);  // -1: full or empty
  bool update(int id, const std::string &name, const std::string &triggers, const std::string &prompt);
  bool setPrompt(int id, const std::string &prompt);
  bool remove(int id);

 private:
  std::vector<Macro> macros_;
  int next_ = 1;
};

// For the instruction: the macros by number, with their phrases ("" when there are none).
std::string macrosSection(const MacroList &macros);
// The text model's request: the companion's voice, ~150 words (a minute read aloud), the prompt.
std::string macroRequest(const Profile &p, const std::string &memories, const Macro &m);
