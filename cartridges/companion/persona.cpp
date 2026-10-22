#include "persona.h"

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
