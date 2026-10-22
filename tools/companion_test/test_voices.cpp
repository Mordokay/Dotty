// uses:
// The voice list Dotty steps through in its settings.
#include <set>
#include <string>

#include "check.h"
#include "voices.h"

int main() {
  CHECK_EQ("voices: all 30", kVoiceCount, 30);
  CHECK_EQ("voices: Zephyr first (the default)", std::string(kVoices[0].name), std::string("Zephyr"));
  CHECK_EQ("voices: Achernar second", std::string(kVoices[1].name), std::string("Achernar"));
  std::set<std::string> names;
  for (const Voice &v : kVoices) names.insert(v.name);
  CHECK_EQ("voices: no repeats", names.size(), size_t(30));
  CHECK_EQ("index: found", voiceIndex("Sulafat"), 29);
  CHECK_EQ("index: unknown", voiceIndex("Zeph"), -1);
  CHECK_EQ("index: a longer name isn't a match", voiceIndex("ZephyrX"), -1);
  // Dotty steps through the favourites picked in the app ("Zephyr,Achernar" by default).
  CHECK_EQ("favourites: next", std::string(nextFavourite("Zephyr,Achernar,Puck", "Zephyr", 1)), std::string("Achernar"));
  CHECK_EQ("favourites: wraps round", std::string(nextFavourite("Zephyr,Achernar,Puck", "Puck", 1)), std::string("Zephyr"));
  CHECK_EQ("favourites: back", std::string(nextFavourite("Zephyr,Achernar,Puck", "Zephyr", -1)), std::string("Puck"));
  CHECK_EQ("favourites: a voice that isn't one starts at the first", std::string(nextFavourite("Kore,Puck", "Zephyr", 1)), std::string("Kore"));
  CHECK_EQ("favourites: unknown names skipped", std::string(nextFavourite("Nope,Puck", "Puck", 1)), std::string("Puck"));
  CHECK_EQ("favourites: none = all voices", std::string(nextFavourite("", "Zephyr", 1)), std::string("Achernar"));
  CHECK_EQ("favourites: count", favouriteCount("Zephyr,Achernar,Nope"), 2);
  return failures ? 1 : 0;
}
