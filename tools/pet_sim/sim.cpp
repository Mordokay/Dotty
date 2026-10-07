// Runs the pet engine (cartridges/pet/pet_engine.*) on a computer with perfect care and prints
// its timeline in the words of the P1 datamine's simulator (timeline.py), so compare.py can
// check the two match. Build: see compare.py.
//   sim <character>[+] <HH:MM>     e.g. sim Tamatchi+ 9:00   (+ = type 1 teen)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../cartridges/pet/pet_engine.h"

static unsigned now = 0;  // minute of the day being simulated
static void trace(const char *what, int value) {
  std::string text = what;
  if (text == "wake") text = "*** TAMA WAKES UP, AGE IS " + std::to_string(value) + " ***";
  if (text == "poop") text = "\xF0\x9F\x9A\xBD";  // the toilet emoji timeline.py prints
  std::printf("%02u:%02u: %s\n", now / 60, now % 60, text.c_str());
}

static const pet::SpeciesInfo &info0(pet::Species s) { return pet::info(s); }

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: sim <character>[+] <HH:MM>\n");
    return 2;
  }
  std::string name = argv[1];
  const bool typeOne = !name.empty() && name.back() == '+';
  if (typeOne) name.pop_back();
  struct { const char *name; pet::Species species; } kNames[] = {
      {"Babytchi", pet::kBaby},     {"Marutchi", pet::kChild},     {"Tamatchi", pet::kTeenA},
      {"Kuchitamatchi", pet::kTeenB}, {"Mametchi", pet::kAdult1},  {"Ginjirotchi", pet::kAdult2},
      {"Maskutchi", pet::kAdult3},  {"Kuchipatchi", pet::kAdult4}, {"Nyorotchi", pet::kAdult5},
      {"Tarakotchi", pet::kAdult6}, {"Bill", pet::kSecret},
  };
  pet::Species species = pet::kEgg;
  for (auto &n : kNames) {
    if (name == n.name) species = n.species;
  }
  if (species == pet::kEgg) {
    std::fprintf(stderr, "unknown character %s\n", name.c_str());
    return 2;
  }
  unsigned h = 0, m = 0;
  std::sscanf(argv[2], "%u:%u", &h, &m);
  const uint32_t start = 1440 * 20000 + h * 60 + m;  // any day
  pet::State s;
  pet::startAs(s, species, info0(species).stage == pet::Stage::Teen && !typeOne, start);
  pet::setTrace(trace);
  // timeline.py runs until the stage ends: evolution, or an adult's 3 x sickness time.
  const pet::SpeciesInfo &info = pet::info(species);
  const bool adult = info.stage == pet::Stage::Adult;
  long awakeLeft = adult || species == pet::kAdult3 ? 3L * info.sickEvery : info.evolveEvery;
  while (awakeLeft > 0) {
    s.lastMinute++;
    now = s.lastMinute % 1440;
    uint32_t events = 0;
    const pet::Species before = s.species;
    pet::step(s, now, events);
    if (!s.asleep) awakeLeft--;
    // Perfect care, at once.
    if (s.disciplineCall) pet::scold(s);
    while (s.sick) pet::medicine(s, events);
    pet::clean(s);
    if (s.asleep && s.lightsOn) pet::setLights(s, false);
    while (s.hunger < 4 && pet::feedMeal(s)) {}
    while (s.happy < 4 && pet::canPlay(s)) pet::finishGame(s, 5);  // games: no effect on timers
    if (s.species != before || s.death != pet::Death::None) break;
  }
  return 0;
}
