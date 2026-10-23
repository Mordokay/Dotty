// uses: cartridges/companion/macro_store.cpp cartridges/companion/script_writer.cpp cartridges/companion/companion_store.cpp
// Macros (the user, 2026-10-23): phrases that make the companion run a prepared prompt through the
// text model (~150 words, in its own voice) and read the result aloud. Shared by all companions.
#include <ArduinoJson.h>

#include "check.h"
#include "macro_store.h"

static bool has(const std::string &s, const std::string &part) {
  return s.find(part) != std::string::npos;
}

int main() {
  MacroList presets = MacroList::withPresets();
  CHECK("presets: four", presets.all().size() == 4);
  CHECK("presets: talk dirty to me first", presets.all()[0].name == "Talk dirty to me" &&
                                               has(presets.all()[0].triggers, "talk dirty to me"));
  CHECK("presets: flirty, never explicit", has(presets.all()[0].prompt, "never explicit") &&
                                               has(presets.all()[0].prompt, "Never describe sexual acts"));
  CHECK("presets: bedtime story, meditation, pep talk",
        presets.all()[1].name == "Bedtime story" && presets.all()[2].name == "Meditation" && presets.all()[3].name == "Pep talk");
  bool fits = true, noGreeting = true;
  for (const Macro &p : presets.all()) {
    fits &= p.prompt.size() <= MacroList::kPromptMax && p.prompt.size() > 300;
    noGreeting &= !has(p.triggers, "good morning");  // said in passing all the time
  }
  CHECK("presets: full prompts within the limit", fits);
  CHECK("presets: no everyday phrases as triggers", noGreeting);

  MacroList m;
  const int a = m.add("Bedtime story", "tell me a bedtime story\nstory time", "A calm bedtime story about the sea.");
  const int b = m.add("Pep talk", "pep talk", "Fire me up for the gym.");
  CHECK("add: ids from 1", a == 1 && b == 2);
  CHECK("find", m.find(2) && m.find(2)->name == "Pep talk" && !m.find(9));
  CHECK("update", m.update(1, "Story", "story time", "A story.") && m.find(1)->name == "Story" && m.find(1)->prompt == "A story.");
  CHECK("update: unknown refused", !m.update(9, "x", "y", "z"));
  CHECK("prompt: replaced alone", m.setPrompt(2, "Push me.") && m.find(2)->prompt == "Push me." && m.find(2)->triggers == "pep talk");
  CHECK("limits: cut", m.update(2, std::string(80, 'n'), std::string(400, 't'), std::string(3000, 'p')) &&
                           m.find(2)->name.size() <= 40 && m.find(2)->triggers.size() <= 300 && m.find(2)->prompt.size() <= 2000);
  CHECK("add: needs a trigger and a prompt", m.add("x", "  ", "p") < 0 && m.add("x", "t", "") < 0);
  CHECK("remove", m.remove(1) && !m.find(1) && !m.remove(1));
  const int c = m.add("Again", "again", "Again.");
  CHECK("ids never reused", c == 3);

  MacroList back;
  CHECK("json: round trip", back.load(m.save()) && back.all().size() == 2 && back.find(3) && back.find(3)->prompt == "Again." &&
                                back.add("n", "t", "p") == 4);
  CHECK("json: broken refused", !back.load("{oops"));

  MacroList full;
  for (int i = 0; i < MacroList::kMax; i++) full.add("m", "t", "p");
  CHECK("full: no more than 12", full.add("m", "t", "p") < 0);

  const std::string section = macrosSection(presets);
  CHECK("section: run_macro with its number", has(section, "call run_macro") && has(section, "1: talk dirty to me / flirt with me") &&
                                                 has(section, "4: give me a pep talk"));
  CHECK("section: none = nothing", macrosSection(MacroList()).empty());

  Profile mia = presetProfile("partner");
  mia.name = "Mia";
  JsonDocument doc;
  deserializeJson(doc, macroRequest(mia, "", presets.all()[0]));
  const std::string sys = doc["systemInstruction"]["parts"][0]["text"] | "";
  const std::string user = doc["contents"][0]["parts"][0]["text"] | "";
  CHECK("request: in Mia's voice, ~150 words", has(sys, "Mia") && has(sys, "About 150 words"));
  CHECK("request: the macro's prompt", user == presets.all()[0].prompt);
  return failures ? 1 : 0;
}
