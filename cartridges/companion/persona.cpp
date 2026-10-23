#include "persona.h"

#include <algorithm>

namespace {

std::string join(const std::vector<std::string> &items, const char *sep) {
  std::string out;
  for (const std::string &i : items) out += (out.empty() ? "" : sep) + i;
  return out;
}

// The character fields, each only when it says something.
std::string character(const Profile &p) {
  std::string s;
  if (!p.personality.empty()) s += " " + p.personality;
  if (!p.traits.empty()) s += " Traits: " + join(p.traits, ", ") + ".";
  if (!p.quirks.empty()) s += " Quirks: " + join(p.quirks, "; ") + ".";
  if (!p.speaking.empty()) s += " How you speak: " + p.speaking;
  if (!p.backstory.empty()) s += " Your story: " + p.backstory;
  return s;
}

// Measured on the device (roles plan Task 1): tone directions in the instruction change how the
// voice sounds (slower, lower, doubtful for "lost and sad").
constexpr const char *kActing =
    " Acting: perform every line like an actor on a stage. Before each reply, decide the emotion the moment calls "
    "for and commit to it fully with your voice: whisper a secret, laugh when something is funny, sigh when tired or "
    "sad, gasp when surprised, hesitate when unsure, speed up when excited, slow down when tender or serious. Small "
    "sounds like \"hmm\", a chuckle or a soft \"oh\" are welcome. Never say stage directions out loud.";

}  // namespace

std::string personaText(const Profile &p) {
  if (p.awake()) return "You are " + p.name + ", a small desk companion who lives on the user's desk." + character(p) + kActing;
  return "You are a small desk companion who has just woken up and can't remember who you are, not even your name. "
         "Deep down you are still yourself:" +
         character(p) +
         " Speak softly and hesitantly, a little lost and sad, not cheerful at all. Ask the user who you are. When they "
         "tell you your name, call set_name with it, react with relief and joy, and become yourself again." +
         kActing;
}

std::string reviewPrompt(const Profile &p) {
  std::string s =
      "(Dotty, not the user: the chat is over.) Go over it: call remember or update_memory for anything worth keeping "
      "that you haven't saved yet.";
  if (p.awake()) {
    s += " Then ask yourself whether this chat really changed you: a new interest, a habit you picked up, a trait that "
         "grew or softened, something new in your story. Most chats change nothing: then don't call evolve. If one did, "
         "call evolve once, with the whole new field (one line per item or sentence), changing just a line or two and "
         "keeping every line marked (pinned) exactly. Write the lines like the ones below: plain, everyday words, "
         "about yourself in the third person (never \"I\"), without the \"- \" marks. If you already "
         "called evolve in this chat, don't call it again.";
  }
  s += " Then call summarize once with one or two sentences about this chat (what you talked about, what the user "
       "said or felt), for your future self.";
  s += " Then reply with just: Bye.";
  if (!p.awake()) return s;
  s += "\n\nYour character now:";
  const char *fields[][2] = {{"personality", "Personality"}, {"traits", "Traits"}, {"quirks", "Quirks"},
                             {"speaking", "Way of speaking (speaking)"}, {"backstory", "Backstory"}};
  for (const auto &f : fields) {
    s += std::string("\n") + f[1] + ":";
    const auto pins = p.pins.find(f[0]);
    std::string text = fieldText(p, f[0]);
    size_t start = 0;
    while (start < text.size()) {
      size_t nl = text.find('\n', start);
      if (nl == std::string::npos) nl = text.size();
      const std::string line = text.substr(start, nl - start);
      const bool pinned = pins != p.pins.end() &&
                          std::find(pins->second.begin(), pins->second.end(), line) != pins->second.end();
      if (!line.empty()) s += "\n- " + line + (pinned ? " (pinned)" : "");
      start = nl + 1;
    }
  }
  return s;
}
