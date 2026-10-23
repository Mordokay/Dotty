#include "macro_store.h"

#include <ArduinoJson.h>

#include <algorithm>

#include "script_writer.h"

namespace {

// Each line trimmed and the empty ones dropped, then cut to the limit.
std::string cleanLines(const std::string &text, int max) {
  std::string out, line;
  for (const char c : text + "\n") {
    if (c != '\n') {
      line += c;
      continue;
    }
    line = cutField(line, max);
    if (!line.empty()) out += (out.empty() ? "" : "\n") + line;
    line.clear();
  }
  return cutField(out, max);
}

}  // namespace

// The presets (written 2026-10-23 at the user's request). Each is a full brief for the text model,
// which also gets the companion's character and memories and is asked for ~150 words; the phrases
// avoid everyday greetings (a "good morning" macro would fire in passing).
MacroList MacroList::withPresets() {
  MacroList m;
  m.add("Talk dirty to me", "talk dirty to me\nflirt with me\nsay something sexy",
        "Speak to me like someone who is completely crazy about me, late at night, when we're finally alone and "
        "you've been missing me all day. Be flirty, affectionate, teasing and suggestive, but never explicit.\n"
        "Start soft and a little playful, as if you just noticed I'm here and can't hide your smile. Tell me two or "
        "three specific things you love about me, using what you know about me, and tease me about one of them. "
        "Then lower your voice and confess what you've been thinking about: being close to me right now, pulling "
        "me in by my shirt, a slow dance in the kitchen with the lights low, your lips almost touching mine, a long "
        "kiss you don't want to end.\n"
        "Let the tension build through closeness, longing, anticipation and what you leave unsaid. Use short "
        "pauses (...), a few whispered lines and playful questions to me. Never describe sexual acts, nudity or body "
        "parts, and nothing graphic: the heat comes from implication.\n"
        "End by teasing me with a promise for later, something sweet and a little daring, like a secret just "
        "between us.");
  m.add("Bedtime story", "tell me a bedtime story\nbedtime story\nhelp me fall asleep",
        "Tell me a calm, cozy bedtime story to help me fall asleep. Invent a gentle story set somewhere peaceful, "
        "like a lighthouse by a quiet sea, a cabin in snowy woods or a garden at dusk, with one or two kind "
        "characters and nothing scary, sad or exciting happening.\n"
        "Use soft, slow sentences full of small sensory details: the sound of rain, warm light, the smell of tea, "
        "the weight of a blanket. Let the story slow down as it goes, with longer pauses (...) near the end.\n"
        "Finish with the characters drifting off to sleep, then wish me goodnight in your own words, tenderly and "
        "quietly.");
  m.add("Meditation", "guide me through a meditation\nhelp me relax\nmeditation time",
        "Guide me through a one-minute relaxation exercise, speaking slowly and calmly.\n"
        "Start by inviting me to get comfortable, let my shoulders drop and close my eyes. Lead me through three "
        "slow breaths: in through the nose, a gentle hold, out through the mouth, with a pause (...) after each "
        "instruction so I have time to follow. Then a short body scan from the forehead down to the feet, letting "
        "each part grow heavy and warm. If a thought comes, tell me to notice it and let it float past like a "
        "cloud.\n"
        "Use simple, kind instructions, never more than one step at a time. End by inviting me to wiggle my "
        "fingers, open my eyes and come back to the room, calm and clear.");
  m.add("Pep talk", "give me a pep talk\nhype me up\nmotivate me",
        "Give me an energetic pep talk to get me moving, like a coach in the last minute before the big game.\n"
        "Make it personal: use what you know about me, my goals, my plans and the people in my life. Remind me of "
        "something I've already done well, name the one thing I should do next, and tell me why I can do it.\n"
        "Use short, punchy sentences that build up in energy, with warmth underneath the push: you believe in me. "
        "No clichés about hustle or grinding. End with a rallying line that makes me want to stand up and start "
        "right now.");
  return m;
}

const Macro *MacroList::find(int id) const {
  for (const Macro &m : macros_)
    if (m.id == id) return &m;
  return nullptr;
}

int MacroList::add(const std::string &name, const std::string &triggers, const std::string &prompt) {
  if (static_cast<int>(macros_.size()) >= kMax) return -1;
  Macro m;
  m.name = cutField(name, kNameMax);
  m.triggers = cleanLines(triggers, kTriggersMax);
  m.prompt = cutField(prompt, kPromptMax);
  if (m.triggers.empty() || m.prompt.empty()) return -1;
  if (m.name.empty()) m.name = m.triggers.substr(0, m.triggers.find('\n'));
  m.id = next_++;
  macros_.push_back(m);
  return m.id;
}

bool MacroList::update(int id, const std::string &name, const std::string &triggers, const std::string &prompt) {
  for (Macro &m : macros_) {
    if (m.id != id) continue;
    const std::string t = cleanLines(triggers, kTriggersMax), p = cutField(prompt, kPromptMax);
    if (t.empty() || p.empty()) return false;
    m.name = cutField(name, kNameMax);
    m.triggers = t;
    m.prompt = p;
    return true;
  }
  return false;
}

bool MacroList::setPrompt(int id, const std::string &prompt) {
  for (Macro &m : macros_) {
    if (m.id != id) continue;
    const std::string p = cutField(prompt, kPromptMax);
    if (p.empty()) return false;
    m.prompt = p;
    return true;
  }
  return false;
}

bool MacroList::remove(int id) {
  const auto it = std::find_if(macros_.begin(), macros_.end(), [&](const Macro &m) { return m.id == id; });
  if (it == macros_.end()) return false;
  macros_.erase(it);
  return true;
}

std::string MacroList::save() const {
  JsonDocument doc;
  doc["next"] = next_;
  JsonArray list = doc["macros"].to<JsonArray>();
  for (const Macro &m : macros_) {
    JsonObject o = list.add<JsonObject>();
    o["id"] = m.id;
    o["name"] = m.name;
    o["triggers"] = m.triggers;
    o["prompt"] = m.prompt;
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool MacroList::load(const std::string &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok || !doc["macros"].is<JsonArrayConst>()) return false;
  macros_.clear();
  for (JsonObjectConst o : doc["macros"].as<JsonArrayConst>()) {
    Macro m;
    m.id = o["id"] | 0;
    m.name = o["name"] | "";
    m.triggers = o["triggers"] | "";
    m.prompt = o["prompt"] | "";
    if (m.id > 0 && !m.triggers.empty() && !m.prompt.empty() && static_cast<int>(macros_.size()) < kMax) macros_.push_back(m);
  }
  next_ = doc["next"] | 1;
  for (const Macro &m : macros_) next_ = std::max(next_, m.id + 1);
  return true;
}

std::string macrosSection(const MacroList &macros) {
  if (macros.all().empty()) return "";
  // "bedtime story" alone once got a reply of its own: phrases count on their own too.
  std::string s = "Macros: when the user's words contain one of these phrases, even on their own (\"bedtime story\"), "
                  "or clearly mean it, always call run_macro with its number instead of answering yourself, and say at "
                  "most a few words; Dotty then gives you a text to read aloud.";
  for (const Macro &m : macros.all()) {
    std::string phrases = m.triggers;
    for (size_t at = 0; (at = phrases.find('\n', at)) != std::string::npos;) phrases.replace(at, 1, " / ");
    s += "\n" + std::to_string(m.id) + ": " + phrases;
  }
  return s;
}

std::string macroRequest(const Profile &p, const std::string &memories, const Macro &m) {
  return writerRequest(p, memories, m.prompt, 60);
}
