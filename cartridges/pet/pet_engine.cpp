#include "pet_engine.h"

namespace pet {
namespace {

// stats.txt (P1 ROM). Teen "type 1" starts at 50 % discipline: applied in enter().
//                     stage          wake  sleep  hu  ha  sick  evolve min max dis shots cd  k  bites
const SpeciesInfo kInfo[kSpeciesCount] = {
    /* egg    */ {Stage::Egg,    -1,   -1,    0,  0,  0,    0,     5,  5,  0,  0,    0,  8, 4},
    /* baby   */ {Stage::Baby,   -1,   -1,    3,  4,  45,   60,    5,  5,  0,  2,    0,  8, 4},
    /* child  */ {Stage::Child,  540,  1200,  50, 60, 990,  1380,  10, 99, 0,  2,    6,  8, 4},
    /* teen A */ {Stage::Teen,   540,  1260,  75, 85, 1656, 2220,  20, 99, 0,  2,    6,  8, 2},
    /* teen B */ {Stage::Teen,   540,  1260,  75, 85, 660,  1380,  20, 99, 0,  2,    6,  8, 4},
    /* adult1 */ {Stage::Adult,  540,  1320,  81, 91, 3900, 4095,  30, 99, 15, 1,    0,  8, 2},
    /* adult2 */ {Stage::Adult,  540,  1320,  81, 91, 2808, 3120,  30, 99, 8,  1,    7,  8, 2},
    /* adult3 */ {Stage::Adult,  660,  1380,  55, 65, 2592, 2880,  30, 99, 0,  1,    7,  11, 2},
    /* adult4 */ {Stage::Adult,  540,  1320,  60, 70, 1170, 1560,  20, 99, 15, 2,    0,  5, 2},
    /* adult5 */ {Stage::Adult,  540,  1320,  60, 70, 360,  780,   10, 99, 8,  3,    7,  8, 4},
    /* adult6 */ {Stage::Adult,  600,  1320,  45, 50, 660,  1440,  20, 99, 0,  2,    7,  8, 2},
    /* secret */ {Stage::Adult,  540,  1320,  81, 91, 3900, 4095,  30, 99, 15, 1,    0,  8, 2},
};

constexpr uint8_t kBabyNapAt = 40, kBabyNapMinutes = 5;  // the baby's nap after hatching
constexpr uint16_t kPoopEvery = 180, kBabyFirstPoop = 15, kBabyPoopEvery = 25;

void (*traceHook)(const char *, int) = nullptr;
void trace(const char *what, int value = 0) {
  if (traceHook) traceHook(what, value);
}

bool isAdult(const State &s) { return info(s.species).stage == Stage::Adult; }

bool asleepAt(const State &s, uint16_t minuteOfDay) {
  const SpeciesInfo &i = info(s.species);
  if (s.species == kBaby) return s.minutesSinceHatch >= kBabyNapAt && s.minutesSinceHatch < kBabyNapAt + kBabyNapMinutes;
  if (i.wake < 0) return false;
  return minuteOfDay >= i.sleep || minuteOfDay < i.wake;
}

void die(State &s, Death cause, uint32_t &events) {
  if (s.death != Death::None) return;
  s.death = cause;
  s.sick = false;
  s.emptyCall = s.lightsCall = s.disciplineCall = 0;
  events |= kDied;
}

// Into a new character (hatching or evolving): its timers, discipline and weight range.
void enter(State &s, Species species, bool typeTwo) {
  s.species = species;
  s.typeTwo = typeTwo;
  const SpeciesInfo &i = info(species);
  s.hungerEvery = i.hungerEvery;
  s.happyEvery = i.happyEvery;
  s.toHunger = i.hungerEvery;
  s.toHappy = i.happyEvery;
  s.toSick = i.sickEvery;
  s.toEvolve = i.evolveEvery;
  s.disciplineCountdown = i.disciplineCountdown;
  s.discipline = (i.stage == Stage::Teen && !typeTwo) ? 8 : i.startDiscipline;
  s.sickThisStage = 0;  // ROM 0x049 is cleared on evolution, and so is the sick flag
  s.sick = false;
  s.sickMinutes = 0;
  s.disciplineCall = 0;
  if (s.weight < i.minWeight) s.weight = i.minWeight;
  if (s.weight > i.maxWeight) s.weight = i.maxWeight;
}

void careMistake(State &s, uint32_t &events) {
  events |= kCareMistake;
  // Adults keep their own count (5 = death), except the adult 3 on its way to the secret one.
  if (isAdult(s) && !(s.species == kAdult3 && s.typeTwo)) {
    if (++s.adultMistakes >= 5) die(s, Death::Neglect, events);
  } else if (s.careMistakes < 15) {
    s.careMistakes++;
  }
}

void getSick(State &s, uint32_t &events) {
  if (s.sick || s.death != Death::None) return;
  s.sick = true;
  s.shotsLeft = info(s.species).shots;
  s.sickMinutes = 0;
  events |= kGotSick;
  if (++s.sickThisStage >= 3) die(s, s.species == kBaby ? Death::Snacks : s.aged ? Death::OldAge : Death::Sickness, events);
}

// Evolution vectors from tama_notes.txt (checked worst → best on care and discipline mistakes).
void evolve(State &s, uint32_t &events) {
  const uint8_t care = s.careMistakes, disc = s.disciplineMistakes;
  switch (s.species) {
    case kBaby:
      enter(s, kChild, false);
      break;
    case kChild:
      if (care < 3) enter(s, kTeenA, disc >= 3);
      else enter(s, kTeenB, disc >= 3);
      break;
    case kTeenA:
      if (!s.typeTwo) {
        if (care < 3) enter(s, disc == 0 ? kAdult1 : disc == 1 ? kAdult2 : kAdult3, false);
        else enter(s, disc < 2 ? kAdult4 : disc < 4 ? kAdult5 : kAdult6, false);
      } else {
        if (care < 4) enter(s, disc < 2 ? kAdult2 : kAdult3, disc >= 2);  // that adult 3 turns secret
        else enter(s, disc < 8 ? kAdult5 : kAdult6, false);
      }
      break;
    case kTeenB:
      if (!s.typeTwo) enter(s, disc < 2 ? kAdult4 : disc == 2 ? kAdult5 : kAdult6, false);
      else enter(s, disc < 6 ? kAdult5 : kAdult6, false);
      break;
    default:  // adults: the timer marks old age (the adult 3 from a type-2 teen becomes secret)
      if (s.species == kAdult3 && s.typeTwo) {
        enter(s, kSecret, false);
        break;
      }
      s.toEvolve = info(s.species).evolveEvery;
      if (!s.aged) {
        s.aged = true;
        s.laysEgg = true;
        events |= kAged;
      }
      return;
  }
  events |= kEvolved;
}

// Every lost heart counts down to a discipline check (utils.py shouldDiscipline).
void disciplineCheck(State &s, uint32_t &events) {
  const uint8_t base = info(s.species).disciplineCountdown;
  if (!base) return;
  if (s.disciplineCountdown == 0 || --s.disciplineCountdown == 0) {
    s.disciplineCountdown = base;
    s.m214 = s.discipline + s.m214 + 1;
    if (s.m214 > 15) {
      s.m214 &= 0xF;  // rolled over: no call this time
    } else if (!s.disciplineCall) {
      trace("Call for Discipline");
      s.disciplineCall = kCallMinutes;
      events |= kCalled;
    }
  }
}

void startEmptyCall(State &s, uint32_t &events) {
  if (s.emptyCall) return;
  s.emptyCall = kCallMinutes;
  events |= kCalled;
}

void loseHeart(uint8_t &meter, State &s, uint32_t &events, const char *what) {
  disciplineCheck(s, events);
  trace(what);
  if (meter == 0) return;
  meter--;
  events |= kHeartLost;
  if (meter == 0) startEmptyCall(s, events);
}

void hatch(State &s, uint32_t &events) {
  enter(s, kBaby, false);
  s.hunger = s.happy = 0;  // babies hatch hungry and bored: they call right away
  s.toHunger = info(kBaby).hungerEvery;
  s.toHappy = 2;
  s.toPoop = kBabyFirstPoop;
  s.minutesSinceHatch = 0;
  s.lightsOn = true;
  events |= kHatched;
  startEmptyCall(s, events);
}

}  // namespace

const SpeciesInfo &info(Species s) {
  return kInfo[s < kSpeciesCount ? s : kEgg];
}

uint8_t ageInterval(uint8_t x) {
  // utils.py do_f3d: rotate the nibbles right twice through carry, then subtract.
  auto rrc = [](uint8_t v, uint8_t &carry) {
    const uint8_t out = static_cast<uint8_t>(((carry << 3) | (v >> 1)) & 0xF);
    carry = v & 1;
    return out;
  };
  uint8_t a = x & 0xF, b = (x >> 4) & 0xF, carry = 0;
  const uint8_t a0 = a, b0 = b;
  b = rrc(b, carry);
  a = rrc(a, carry);
  b = rrc(b, carry);
  a = rrc(a, carry);
  b &= 0x3;
  const uint8_t lower = (a0 - a) & 0xF;
  const uint8_t borrow = a0 < a;
  const uint8_t upper = (b0 - b - borrow) & 0xF;
  return static_cast<uint8_t>((upper << 4) | lower);
}

void startAs(State &s, Species species, bool typeTwo, uint32_t nowMinute) {
  uint32_t events = 0;
  if (species == kBaby) {
    hatch(s, events);
  } else {
    enter(s, species, typeTwo);
    s.toPoop = kPoopEvery;
  }
  s.hunger = s.happy = 4;
  s.emptyCall = 0;
  s.lastMinute = nowMinute;
  s.asleep = asleepAt(s, nowMinute % 1440);
}

void setTrace(void (*hook)(const char *, int)) {
  traceHook = hook;
}

void newEgg(State &s, uint32_t nowMinute) {
  const uint32_t generation = s.version == kStateVersion && s.bornMinute ? s.generation + 1 : 1;
  s = State();
  s.generation = generation;
  s.lastMinute = nowMinute;
  s.bornMinute = nowMinute;
}

void step(State &s, uint16_t minuteOfDay, uint32_t &events) {
  if (s.death != Death::None || s.paused) return;
  if (s.species == kEgg) {
    if (++s.eggMinutes >= kHatchMinutes) hatch(s, events);
    return;
  }
  const bool wasAsleep = s.asleep;
  if (s.species == kBaby) s.minutesSinceHatch++;
  s.asleep = asleepAt(s, minuteOfDay);
  if (s.asleep && !wasAsleep) {
    events |= kFellAsleep;
    s.lightsMistakeTonight = false;
    s.emptyCall = s.disciplineCall = 0;
    if (s.lightsOn) {
      s.lightsCall = kLightsMinutes;
      events |= kCalled;
    }
  } else if (!s.asleep && wasAsleep) {
    events |= kWokeUp;
    s.lightsOn = true;  // morning: the light comes back on by itself
    s.lightsCall = 0;
    if (s.age < 99) s.age++;
    trace("wake", s.age);
    if (s.aged) {  // old age: hearts drop faster every day (ROM 0xF3D)
      s.hungerEvery = ageInterval(s.hungerEvery);
      s.happyEvery = ageInterval(s.happyEvery);
      s.agedDays++;
    }
  }

  if (s.asleep) {
    if (s.lightsCall) {
      if (!s.lightsOn) s.lightsCall = 0;
      else if (--s.lightsCall == 0 && !s.lightsMistakeTonight) {
        s.lightsMistakeTonight = true;
        careMistake(s, events);
      }
    }
    return;
  }

  // Awake: everything counts down.
  if (!s.sick && --s.toEvolve == 0) {
    trace("Time to Evolution reached");
    evolve(s, events);
  }
  if (s.death != Death::None) return;
  if (!s.sick) {
    if (--s.toSick == 0) {
      trace("Time to Sickness reached");
      s.toSick = info(s.species).sickEvery;
      getSick(s, events);
    }
  } else if (++s.sickMinutes >= kUntreatedMinutes) {
    die(s, s.aged ? Death::OldAge : Death::Sickness, events);  // ★
    return;
  }
  if (s.death != Death::None) return;

  if (--s.toHunger == 0) {
    s.toHunger = s.hungerEvery;
    loseHeart(s.hunger, s, events, "Hungry Heart Decrement");
  }
  if (--s.toHappy == 0) {
    s.toHappy = s.happyEvery;
    loseHeart(s.happy, s, events, "Happy Heart Decrement");
  }
  if (--s.toPoop == 0) {
    s.toPoop = s.species == kBaby ? kBabyPoopEvery : kPoopEvery;
    if (s.poops < 4) s.poops++;
    trace("poop");
    events |= kPooped;
  }
  // ★ Poop left too long, or too much of it, makes it sick.
  if (s.poops) {
    if (++s.poopMinutes >= kPoopSickMinutes || s.poops >= kPoopSickCount) {
      s.poopMinutes = 0;
      getSick(s, events);
    }
  } else {
    s.poopMinutes = 0;
  }
  // ★ Starvation: an empty meter for too long.
  if (s.hunger == 0 || s.happy == 0) {
    if (++s.emptyMinutes >= kStarveMinutes) {
      die(s, Death::Starved, events);
      return;
    }
  } else {
    s.emptyMinutes = 0;
  }

  // Calls not answered in time are mistakes.
  if (s.emptyCall) {
    if (s.hunger > 0 && s.happy > 0) s.emptyCall = 0;
    else if (--s.emptyCall == 0) careMistake(s, events);
  }
  if (s.disciplineCall && --s.disciplineCall == 0) {
    if (s.disciplineMistakes < 15) s.disciplineMistakes++;
    events |= kDisciplineMistake;
  }
}

uint32_t catchUp(State &s, uint32_t nowMinute) {
  uint32_t events = 0;
  if (s.paused || s.death != Death::None || nowMinute <= s.lastMinute) {
    if (nowMinute > s.lastMinute) s.lastMinute = nowMinute;  // paused: time passes without it
    return 0;
  }
  while (s.lastMinute < nowMinute) {
    s.lastMinute++;
    step(s, static_cast<uint16_t>(s.lastMinute % 1440), events);
    if (s.death != Death::None) {
      s.lastMinute = nowMinute;
      break;
    }
  }
  return events;
}

bool feedMeal(State &s) {
  if (stage(s) == Stage::Egg || stage(s) == Stage::Dead || s.asleep || s.sick || s.hunger >= 4) return false;
  s.hunger++;
  if (s.weight < info(s.species).maxWeight) s.weight++;
  return true;
}

bool feedSnack(State &s, uint32_t &events) {
  if (stage(s) == Stage::Egg || stage(s) == Stage::Dead || s.asleep || s.sick) return false;
  if (s.happy < 4) s.happy++;
  const uint8_t max = info(s.species).maxWeight;
  s.weight = s.weight + 2 > max ? max : s.weight + 2;
  // A snack takes a minute off the time to the next sickness (too many make it sick).
  if (s.toSick > 1) s.toSick--;
  else {
    s.toSick = info(s.species).sickEvery;
    getSick(s, events);
  }
  return true;
}

void setLights(State &s, bool on) {
  s.lightsOn = on;
  if (!on) s.lightsCall = 0;
}

bool medicine(State &s, uint32_t &events) {
  if (!s.sick) return false;
  if (s.shotsLeft > 0) s.shotsLeft--;
  if (s.shotsLeft == 0) {
    s.sick = false;
    s.sickMinutes = 0;
    s.toSick = info(s.species).sickEvery;
    events |= kCured;
  }
  return true;
}

bool clean(State &s) {
  if (!s.poops) return false;
  s.poops = 0;
  s.poopMinutes = 0;
  return true;
}

bool scold(State &s) {
  if (!s.disciplineCall) return false;
  s.disciplineCall = 0;
  s.discipline = s.discipline + 4 > 15 ? 15 : s.discipline + 4;
  return true;
}

bool canPlay(const State &s) {
  const Stage st = stage(s);
  return st != Stage::Egg && st != Stage::Dead && !s.asleep && !s.sick;
}

bool roundWon(const State &s, uint8_t random16) {
  return (random16 & 0xF) < 16 - info(s.species).winK;
}

void finishGame(State &s, int wins) {
  const uint8_t min = info(s.species).minWeight;
  if (s.weight > min) s.weight--;
  if (wins >= 3 && s.happy < 4) s.happy++;
}

void setPaused(State &s, bool on, uint32_t nowMinute) {
  if (on == s.paused) return;
  s.paused = on;
  s.lastMinute = nowMinute;  // resuming carries on from here: the paused time never happened
}

uint8_t disciplinePercent(const State &s) {
  const int pct = ((s.discipline + 1) / 4) * 25;
  return pct > 100 ? 100 : pct;
}

}  // namespace pet
