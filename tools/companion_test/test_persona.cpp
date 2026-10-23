// uses: cartridges/companion/persona.cpp cartridges/companion/companion_store.cpp cartridges/companion/memory_store.cpp
// Who the companion is told it is: awake (named) or just woken up, plus how to act every line.
#include <string>

#include "check.h"
#include "persona.h"

static bool has(const std::string &s, const char *part) {
  return s.find(part) != std::string::npos;
}

int main() {
  Profile p = presetProfile("friend");
  p.name = "Luna";
  p.traits = {"stubborn", "curious"};
  p.backstory = "";
  const std::string awake = personaText(p);
  CHECK("awake: the name first", awake.rfind("You are Luna,", 0) == 0);
  CHECK("awake: personality", has(awake, p.personality.c_str()));
  CHECK("awake: traits", has(awake, "Traits: stubborn, curious."));
  CHECK("awake: quirks", has(awake, "Quirks: invents nicknames; gets excited about snacks."));
  CHECK("awake: way of speaking", has(awake, "How you speak: "));
  CHECK("awake: an empty backstory left out", !has(awake, "Your story"));
  CHECK("awake: no waking up", !has(awake, "can't remember"));
  CHECK("awake: acting", has(awake, "commit to it") && has(awake, "Never say stage directions"));

  const Profile asleep = presetProfile("partner");
  const std::string waking = personaText(asleep);
  CHECK("asleep: no name, can't remember", has(waking, "can't remember who you are") && !has(waking, "You are Mia"));
  CHECK("asleep: the tone that worked", has(waking, "Speak softly and hesitantly, a little lost and sad"));
  CHECK("asleep: asks, then set_name", has(waking, "set_name"));
  CHECK("asleep: still themselves underneath", has(waking, asleep.personality.c_str()));
  CHECK("asleep: acting too", has(waking, "commit to it"));

  // The end-of-chat review: memories, then maybe one slow change, pinned lines marked.
  Profile partner = presetProfile("partner");
  partner.name = "Mia";
  partner.traits = {"witty", "horny"};
  partner.pins["traits"] = {"horny"};
  const std::string review = reviewPrompt(partner);
  CHECK("review: memories first", has(review, "remember or update_memory"));
  CHECK("review: evolve, rarely", has(review, "evolve") && has(review, "Most chats change nothing"));
  CHECK("review: pinned lines marked", has(review, "- horny (pinned)") && has(review, "- witty\n"));
  CHECK("review: not twice in a chat", has(review, "already called evolve in this chat"));
  CHECK("review: same style as the sheet", has(review, "third person"));
  CHECK("review: ends with Bye", has(review, "Bye."));
  CHECK("review: asleep, no evolving", !has(reviewPrompt(presetProfile("coach")), "evolve"));
  return failures ? 1 : 0;
}
