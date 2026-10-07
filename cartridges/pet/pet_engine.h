#pragma once

#include <stddef.h>
#include <stdint.h>

// The pet's rules: the original 1996 Tamagotchi (P1), rebuilt from a ROM datamine
// (rhubarbtart.neocities.org/p1hackinglog: stats.txt, tama_notes.txt, timeline.py, utils.py).
// Plain C++ with no Arduino, so tools/pet_sim can run it on a computer and check it against
// the datamine's own simulator. Our species have their own names and art; the numbers are the
// P1's. Rules the datamine couldn't settle are marked ★ and kept as constants below.
//
// Time: everything counts awake minutes; while the pet sleeps (and while paused) nothing grows,
// no hearts drop, no poop. Age +1 each morning when it wakes up.
namespace pet {

enum class Stage : uint8_t { Egg, Baby, Child, Teen, Adult, Dead };

// The P1 characters, in its order (ROM index 0x05D = this + 1 for Baby…Secret).
enum Species : uint8_t {
  kEgg,
  kBaby,     // Babytchi
  kChild,    // Marutchi
  kTeenA,    // Tamatchi (good care)
  kTeenB,    // Kuchitamatchi (poor care)
  kAdult1,   // Mametchi: the best adult
  kAdult2,   // Ginjirotchi
  kAdult3,   // Maskutchi (from a type-2 teen A it becomes the secret one)
  kAdult4,   // Kuchipatchi
  kAdult5,   // Nyorotchi
  kAdult6,   // Tarakotchi
  kSecret,   // Bill / Oyajitchi
  kSpeciesCount
};

struct SpeciesInfo {
  Stage stage;
  int16_t wake, sleep;          // minute of the day (-1: the baby only naps)
  uint8_t hungerEvery, happyEvery;  // minutes per lost heart
  uint16_t sickEvery;           // awake minutes to a natural sickness
  uint16_t evolveEvery;         // awake minutes to evolve (adults: to old age)
  uint8_t minWeight, maxWeight;
  uint8_t startDiscipline;      // 0..15
  uint8_t shots;                // medicine doses to get well
  uint8_t disciplineCountdown;  // heart drops between discipline checks (0: never calls)
  uint8_t winK;                 // a game round is won when rand(16) < 16 - winK
  uint8_t bites;                // eating animation length
};
const SpeciesInfo &info(Species s);

enum class Death : uint8_t { None, Sickness, OldAge, Neglect, Starved, Snacks };

// Things that happened, for sounds and screens (bit flags, collected since the last take).
enum Event : uint32_t {
  kHatched = 1u << 0,
  kEvolved = 1u << 1,
  kPooped = 1u << 2,
  kGotSick = 1u << 3,
  kCalled = 1u << 4,       // the attention icon lit (hunger/happy empty, lights, discipline)
  kDied = 1u << 5,
  kFellAsleep = 1u << 6,
  kWokeUp = 1u << 7,
  kHeartLost = 1u << 8,
  kCareMistake = 1u << 9,
  kDisciplineMistake = 1u << 10,
  kCured = 1u << 11,
  kAged = 1u << 12,        // an adult reached old age
};

// Everything that has to survive a restart (saved as-is; bump kStateVersion on changes).
constexpr uint8_t kStateVersion = 1;
struct State {
  uint8_t version = kStateVersion;
  Species species = kEgg;
  bool typeTwo = false;      // teen type 2 (no discipline); a type-2 teen A's adult 3 turns secret
  Death death = Death::None;
  uint8_t hunger = 0, happy = 0;   // hearts 0..4
  uint8_t discipline = 0;          // 0..15 (shown as 25 % steps)
  uint8_t weight = 5, age = 0;
  uint8_t careMistakes = 0;        // child + teen, decides evolution
  uint8_t disciplineMistakes = 0;  // child + teen, decides evolution
  uint8_t adultMistakes = 0;       // 5 kill an adult
  uint8_t sickThisStage = 0;       // 3 kill it
  bool sick = false;
  uint8_t shotsLeft = 0;
  uint8_t poops = 0;
  bool lightsOn = true;
  bool asleep = false;
  bool aged = false;               // adult past its old-age timer (hearts drop faster each day)
  uint8_t agedDays = 0;
  bool laysEgg = false;            // cosmetic: an old adult lays an egg before it dies
  // Countdowns (awake minutes).
  uint16_t toHunger = 0, toHappy = 0, toPoop = 0, toSick = 0, toEvolve = 0;
  uint8_t hungerEvery = 0, happyEvery = 0;  // shrink with old age
  uint8_t disciplineCountdown = 0, m214 = 0;
  // Attention calls: minutes left to answer (0 = no call).
  uint8_t emptyCall = 0, lightsCall = 0, disciplineCall = 0;
  bool lightsMistakeTonight = false;
  uint16_t emptyMinutes = 0;       // ★ starvation counter
  uint16_t sickMinutes = 0;        // ★ untreated sickness counter
  uint16_t poopMinutes = 0;        // ★ poop left on screen
  uint16_t minutesSinceHatch = 0;  // the baby's nap
  uint8_t eggMinutes = 0;
  bool paused = false;
  uint32_t lastMinute = 0;         // local time in minutes since 1970: simulated up to here
  uint32_t bornMinute = 0;
  uint32_t generation = 1;
};

// ★ Rules the datamine couldn't settle (tunable).
constexpr uint8_t kCallMinutes = 15;          // to answer a call before it's a mistake (ROM ~14)
constexpr uint8_t kLightsMinutes = 15;        // lights left on after it fell asleep (ROM guess 64)
constexpr uint16_t kStarveMinutes = 720;      // an empty meter this long: death
constexpr uint16_t kUntreatedMinutes = 720;   // sick and untreated this long: death
constexpr uint16_t kPoopSickMinutes = 120;    // poop left this long: sick
constexpr uint8_t kPoopSickCount = 4;         // or this many poops at once
constexpr uint8_t kHatchMinutes = 5;

// --- time ---
void newEgg(State &s, uint32_t nowMinute);
// Lives every minute from s.lastMinute to nowMinute (local time); returns the events. Paused:
// time passes without the pet.
uint32_t catchUp(State &s, uint32_t nowMinute);
void step(State &s, uint16_t minuteOfDay, uint32_t &events);

// --- care (each returns false when refused, e.g. full, asleep, sick, nothing to do) ---
bool feedMeal(State &s);
bool feedSnack(State &s, uint32_t &events);
void setLights(State &s, bool on);
bool medicine(State &s, uint32_t &events);  // one shot
bool clean(State &s);
bool scold(State &s);                       // only works during a discipline call
// The game: a round is decided before you choose (as on the P1); `random16` is 0..15.
bool canPlay(const State &s);
bool roundWon(const State &s, uint8_t random16);
void finishGame(State &s, int wins);        // −1 weight, +1 happy for 3+ wins of 5
void setPaused(State &s, bool on, uint32_t nowMinute);

// --- reading ---
inline Stage stage(const State &s) { return s.death != Death::None ? Stage::Dead : info(s.species).stage; }
inline bool calling(const State &s) { return s.emptyCall || s.lightsCall || s.disciplineCall; }
uint8_t disciplinePercent(const State &s);  // 0, 25, 50, 75, 100

// --- tests and developer aids ---
// Starts as a given character (as if it had just evolved), e.g. to check one stage's timeline.
void startAs(State &s, Species species, bool typeTwo, uint32_t nowMinute);
// Called with what happened, in the datamine simulator's words (tools/pet_sim compares them).
void setTrace(void (*trace)(const char *what, int value));

// The P1's old-age step for a heart interval (ROM routine at 0xF3D): x − x/4 in nibbles.
uint8_t ageInterval(uint8_t minutes);

}  // namespace pet
