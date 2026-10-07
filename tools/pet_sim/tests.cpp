// Rule tests for the pet engine beyond the perfect-care timelines compare.py checks: every
// evolution branch of the datamine's table, each way to die, pause, discipline, food, game.
#include <cstdio>
#include <cstdlib>

#include "../../cartridges/pet/pet_engine.h"

using namespace pet;
static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static const uint32_t kDay = 1440 * 20000;  // midnight of some day

// Lives awake minutes with care that answers nothing on its own; `care` runs every minute.
template <typename Care>
static uint32_t live(State &s, uint32_t minutes, Care care) {
  uint32_t all = 0;
  for (uint32_t i = 0; i < minutes && s.death == Death::None; i++) {
    s.lastMinute++;
    uint32_t events = 0;
    step(s, s.lastMinute % 1440, events);
    all |= events;
    care(s);
  }
  return all;
}
static void perfect(State &s) {
  uint32_t e = 0;
  if (s.disciplineCall) scold(s);
  while (s.sick) medicine(s, e);
  clean(s);
  if (s.asleep && s.lightsOn) setLights(s, false);
  while (s.hunger < 4 && feedMeal(s)) {}
  while (s.happy < 4 && canPlay(s)) finishGame(s, 5);
}

// Evolve a teen with given mistakes: returns the adult.
static Species adultFrom(Species teen, bool typeTwo, uint8_t care, uint8_t disc) {
  State s;
  startAs(s, teen, typeTwo, kDay + 9 * 60);
  s.careMistakes = care;
  s.disciplineMistakes = disc;
  s.toEvolve = 1;
  uint32_t e = 0;
  s.lastMinute++;
  step(s, s.lastMinute % 1440, e);
  return s.species;
}
static Species teenFrom(uint8_t care, uint8_t disc, bool &typeTwo) {
  State s;
  startAs(s, kChild, false, kDay + 9 * 60);
  s.careMistakes = care;
  s.disciplineMistakes = disc;
  s.toEvolve = 1;
  uint32_t e = 0;
  s.lastMinute++;
  step(s, s.lastMinute % 1440, e);
  typeTwo = s.typeTwo;
  return s.species;
}

int main() {
  // --- evolution table (tama_notes.txt) ---
  bool t2;
  CHECK(teenFrom(0, 0, t2) == kTeenA && !t2);
  CHECK(teenFrom(2, 3, t2) == kTeenA && t2);
  CHECK(teenFrom(3, 2, t2) == kTeenB && !t2);
  CHECK(teenFrom(5, 5, t2) == kTeenB && t2);
  CHECK(adultFrom(kTeenA, false, 0, 0) == kAdult1);
  CHECK(adultFrom(kTeenA, false, 2, 1) == kAdult2);
  CHECK(adultFrom(kTeenA, false, 2, 2) == kAdult3);
  CHECK(adultFrom(kTeenA, false, 3, 1) == kAdult4);
  CHECK(adultFrom(kTeenA, false, 3, 3) == kAdult5);
  CHECK(adultFrom(kTeenA, false, 3, 4) == kAdult6);
  CHECK(adultFrom(kTeenA, true, 3, 1) == kAdult2);
  CHECK(adultFrom(kTeenA, true, 3, 2) == kAdult3);
  CHECK(adultFrom(kTeenA, true, 4, 7) == kAdult5);
  CHECK(adultFrom(kTeenA, true, 4, 8) == kAdult6);
  CHECK(adultFrom(kTeenB, false, 9, 1) == kAdult4);
  CHECK(adultFrom(kTeenB, false, 9, 2) == kAdult5);
  CHECK(adultFrom(kTeenB, false, 9, 3) == kAdult6);
  CHECK(adultFrom(kTeenB, true, 9, 5) == kAdult5);
  CHECK(adultFrom(kTeenB, true, 9, 6) == kAdult6);
  // The secret one: adult 3 from a type-2 teen A, after its 2880 awake minutes.
  {
    State s;
    startAs(s, kAdult3, true, kDay + 11 * 60);
    live(s, 4 * 1440, perfect);
    CHECK(s.species == kSecret);
    State n;
    startAs(n, kAdult3, false, kDay + 11 * 60);
    live(n, 4 * 1440, perfect);
    CHECK(n.species == kAdult3 && n.aged);
  }
  // Starting discipline: type-1 teens 50 %, type-2 0 %, best adult 100 %.
  {
    State s;
    startAs(s, kTeenA, false, kDay);
    CHECK(disciplinePercent(s) == 50);
    startAs(s, kTeenA, true, kDay);
    CHECK(disciplinePercent(s) == 0);
    startAs(s, kAdult1, false, kDay);
    CHECK(disciplinePercent(s) == 100);
  }

  // --- a whole life from the egg, perfect care: hatches in 5 minutes, ends as the best adult ---
  {
    State s;
    newEgg(s, kDay + 8 * 60 + 55);
    uint32_t e = live(s, 5, perfect);
    CHECK((e & kHatched) && s.species == kBaby);
    live(s, 30 * 1440, perfect);
    CHECK(s.death == Death::OldAge);
    CHECK(s.careMistakes == 0 && s.disciplineMistakes == 0);
    std::printf("perfect life: species %d, age %d, weight %d\n", s.species, s.age, s.weight);
  }

  // --- deaths ---
  {  // 5 care mistakes as an adult: never feed.
    State s;
    startAs(s, kAdult2, false, kDay + 9 * 60);
    live(s, 20 * 1440, [](State &x) {
      uint32_t e = 0;
      while (x.sick) medicine(x, e);
      clean(x);
      if (x.disciplineCall) scold(x);
      while (x.happy < 4 && canPlay(x)) finishGame(x, 5);
    });
    CHECK(s.death == Death::Neglect || s.death == Death::Starved);
  }
  {  // 3 sicknesses in one stage: never give medicine.
    State s;
    startAs(s, kAdult5, false, kDay + 9 * 60);  // sick every 360 min
    live(s, 10 * 1440, [](State &x) {
      if (x.disciplineCall) scold(x);
      clean(x);
      while (x.hunger < 4 && feedMeal(x)) {}
      while (x.happy < 4 && canPlay(x)) finishGame(x, 5);
    });
    CHECK(s.death == Death::Sickness);
  }
  {  // starvation: nothing at all for long enough.
    State s;
    startAs(s, kChild, false, kDay + 9 * 60);
    s.hunger = 0;
    live(s, 2 * 1440, [](State &x) {  // cleaned and treated, never fed (asleep minutes don't count)
      uint32_t e = 0;
      clean(x);
      while (x.sick) medicine(x, e);
    });
    CHECK(s.death == Death::Starved);
  }
  {  // too many snacks kill the baby (its sickness timer is 45 minutes).
    State s;
    startAs(s, kBaby, false, kDay + 9 * 60);
    uint32_t e = 0;
    for (int i = 0; i < 200 && s.death == Death::None; i++) {
      feedSnack(s, e);
      while (!s.sick && s.death == Death::None && feedSnack(s, e)) {}
      while (s.sick && medicine(s, e)) {}
    }
    CHECK(s.death == Death::Snacks);
  }

  // --- care mistakes ---
  {  // an empty heart not fixed within 15 minutes = 1 care mistake (child).
    State s;
    startAs(s, kChild, false, kDay + 10 * 60);
    s.hunger = 1;
    s.toHunger = 1;
    live(s, kCallMinutes + 1, [](State &) {});
    CHECK(s.careMistakes == 1);
  }
  {  // lights left on after it fell asleep = 1 care mistake, once a night.
    State s;
    startAs(s, kChild, false, kDay + 19 * 60 + 50);
    live(s, 60, [](State &x) {
      while (x.hunger < 4 && feedMeal(x)) {}
      while (x.happy < 4 && canPlay(x)) finishGame(x, 5);
    });
    CHECK(s.asleep && s.careMistakes == 1);
  }

  // --- discipline ---
  {
    State s;
    startAs(s, kChild, false, kDay + 10 * 60);
    CHECK(!scold(s));  // nothing to scold
    s.disciplineCall = kCallMinutes;
    CHECK(scold(s) && s.discipline == 4);
    s.disciplineCall = 3;
    live(s, 4, [](State &) {});
    CHECK(s.disciplineMistakes == 1);
  }

  // --- food and game ---
  {
    State s;
    startAs(s, kChild, false, kDay + 10 * 60);
    s.hunger = 4;
    CHECK(!feedMeal(s));  // full: refused
    s.hunger = 3;
    const uint8_t w = s.weight;
    CHECK(feedMeal(s) && s.hunger == 4 && s.weight == w + 1);
    uint32_t e = 0;
    s.happy = 2;
    CHECK(feedSnack(s, e) && s.happy == 3 && s.weight == w + 3);
    finishGame(s, 2);
    CHECK(s.happy == 3 && s.weight == w + 2);
    finishGame(s, 3);
    CHECK(s.happy == 4);
    CHECK(roundWon(s, 7) && !roundWon(s, 8));  // k = 8: 50 %
    State lucky;
    startAs(lucky, kAdult4, false, kDay);
    CHECK(roundWon(lucky, 10) && !roundWon(lucky, 11));  // k = 5
  }

  // --- pause: time passes without it ---
  {
    State s;
    startAs(s, kChild, false, kDay + 10 * 60);
    const State before = s;
    setPaused(s, true, s.lastMinute);
    CHECK(catchUp(s, s.lastMinute + 3 * 1440) == 0);
    setPaused(s, false, s.lastMinute);
    CHECK(s.hunger == before.hunger && s.toEvolve == before.toEvolve && s.age == before.age);
  }

  // --- the ROM's old-age step ---
  {
    const uint8_t expect[] = {81, 61, 46, 35, 27, 21, 16, 12, 9, 7, 6, 5};
    uint8_t x = 81;
    bool ok = true;
    for (uint8_t want : expect) {
      ok = ok && x == want;
      x = ageInterval(x);
    }
    CHECK(ok);
  }

  std::printf(failures ? "%d FAILED\n" : "all rule tests pass\n", failures);
  return failures ? 1 : 0;
}
