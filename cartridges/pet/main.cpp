// Pet: a virtual pet with the original 1996 Tamagotchi's rules (pet_engine.h, from a ROM
// datamine), our own species and art (images/sprites.h, tools/pet_art.py), touch controls.
//
// Home: the pet in the middle (walking about a little after you touch Dotty), its poop on the
// right, and the P1's eight functions as icons: Food, Light, Game, Medicine on top; Clean,
// Meter, Discipline and the attention bell below (the bell is dark while it calls). BOOT goes
// back. Locked, the lock screen shows everything at a glance and the pet keeps living (the
// engine steps once a minute when Dotty wakes up). The pet only lives while this cartridge
// runs: power off or another cartridge pauses it, and it resumes where it was.

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Preferences.h>
#include <SD_MMC.h>

#include "audio_player.h"
#include "cartridge.h"
#include <vector>
#include "core_ble.h"
#include "images/sleep_panda.h"
#include "images/sprites.h"
#include "log.h"
#include "nav_bar.h"
#include "pet_engine.h"
#include "shell.h"
#include "storage.h"
#include "ui.h"

DOTTY_CARTRIDGE("pet", "Pet", "0.3.2");

namespace {

using shell::epd;
constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

// Home layout: icon bars top and bottom, the pet between.
constexpr int16_t kBarH = 40;
constexpr int16_t kPlayTop = kBarH, kPlayBottom = kW - kBarH;
constexpr int kPetScale = 2;  // 48 px art → 96 px
constexpr int16_t kPetPx = art::kPetSize * kPetScale;
constexpr int16_t kPetY = kPlayTop + (kPlayBottom - kPlayTop - kPetPx) / 2;
constexpr uint32_t kIdleFrameMs = 1500;    // walking about after a touch…
constexpr uint32_t kIdleForMs = 20000;     // …for this long, then it holds still (panel wear)
constexpr uint32_t kAnimFrameMs = 700;     // animation frames (a partial refresh is ~0.6 s)
constexpr uint32_t kSaveEveryMinutes = 10;

enum class Screen { Home, Food, Light, GameMenu, Game, Meter, Anim };
enum Function { kFood, kLight, kGame, kMedicine, kClean, kMeter, kDiscipline, kAttention };
const art::Icon kBarIcons[8] = {art::kFoodIcon,  art::kBulbIcon,  art::kGameIcon,       art::kSyringeIcon,
                                art::kDuckIcon,  art::kMeterIcon, art::kDisciplineIcon, art::kBellIcon};

pet::State state;
Screen screen = Screen::Home;
bool sdReady = false;
bool redraw = true;
bool fullRedraw = false;
uint32_t pendingEvents = 0;  // since the screen last looked
uint32_t lastTouch = 0;
uint32_t nextIdleFrame = 0;
int16_t petX = (kW - kPetPx) / 2;
bool bobFrame = false;
uint32_t lastSavedMinute = 0;
bool fastForward = false;  // developer aid (serial F): a minute every pass

// A short animation: frames drawn one after another, then back home.
enum class Anim { None, Eat, Snack, Refuse, Medicine, Clean, Scold, NoScold, Evolve, Hatch, GameEnd, Nothing };
struct AnimState {
  Anim kind = Anim::None;
  int frame = 0, frames = 0;
  uint32_t next = 0;
  bool flag = false;  // per animation (e.g. a game round won)
} anim;

// The games. Each is 5 rounds and ends the P1's way (pet::finishGame: 3+ wins = +1 happy,
// always -1 weight), so the rules stay the 1996 ones; only how a round is won differs.
enum class GameKind : uint8_t { LeftRight, HigherLower, Shells, Memory, Clap, kCount };
const char *const kGameNames[] = {"Left/Right", "High/Low", "Shells", "Memory", "Clap!"};
enum class Phase : uint8_t { Show, Input, Result };  // watch, play, see how it went
struct GameState {
  GameKind kind = GameKind::LeftRight;
  int round = 0, wins = 0;
  uint8_t results = 0;  // bit per round: won
  Phase phase = Phase::Input;
  bool lastWon = false;
  uint32_t next = 0;  // when the timed step (showing, the result) moves on
  // Left or right
  bool guessedLeft = false;
  // Higher or lower (the P2's game)
  uint8_t number = 5, nextNumber = 5;
  bool guessedHigher = false;
  // Shells: which cup hides the pet, and the swaps shown one by one
  uint8_t petAt = 0, picked = 0;
  uint8_t swaps[6][2] = {};
  int swapCount = 0, swapIndex = -1;
  // Memory: a sequence of quarters to repeat
  uint8_t seq[8] = {};
  int seqLen = 0, seqIndex = 0, inputIndex = 0;
  int8_t lit = -1;
  uint32_t litUntil = 0;
  // Clap the rhythm: Dotty beeps a pattern, the microphone times your claps
  uint8_t target = 0, claps = 0;  // beats in the pattern, claps heard
  uint16_t beatAt[8] = {}, clapAt[8] = {};  // ms from the first beat / the first clap
  uint16_t patternMs = 0;
  bool listening = false, loud = false;
  uint32_t listenStart = 0, lastClap = 0;
  int32_t floorSum = 0;
  int floorBlocks = 0, threshold = 0;
} game;
uint8_t hiloNumber = 5;  // Higher or lower carries its number over, as on the P2

void notifyChanged();  // the app's pet.changed event (below)

// ---------- sound: synthesised chirps (AudioPlayer::playNotes) ----------

AudioPlayer player;
bool audioReady = false;
// Three kinds, each with its own switch: clicks (every tap), actions (what you do: eating, the
// game, cleaning…) and alerts (what the pet does by itself: calls, poop, sickness, hatching,
// growing up, death). `on` mutes all of them; quiet hours mute only alerts, since clicks and
// actions only happen while you're touching Dotty.
enum class SoundKind { Click, Action, Alert };
struct Sound {
  bool on = true;
  bool clicks = true, actions = true, alerts = true;
  uint8_t volume = 60;
  uint16_t quietFrom = 23 * 60 + 30, quietTo = 8 * 60;  // minutes of the day; equal = never quiet
} sound;

using N = AudioPlayer::Note;
#define TUNE(name, ...) const N name[] = {__VA_ARGS__}
TUNE(kTuneCall, {84, 70}, {0, 40}, {88, 110});
TUNE(kTuneEat, {76, 60}, {0, 50}, {76, 60}, {0, 50}, {79, 90});
TUNE(kTuneHappy, {72, 80}, {76, 80}, {79, 80}, {84, 180});
TUNE(kTuneWin, {79, 70}, {84, 140});
TUNE(kTuneLose, {67, 100}, {60, 200});
TUNE(kTuneEvolve, {72, 100}, {76, 100}, {79, 100}, {84, 100}, {79, 100}, {84, 320});
TUNE(kTuneHatch, {84, 60}, {0, 30}, {84, 60}, {0, 30}, {91, 220});
TUNE(kTunePoop, {55, 80}, {50, 160});
TUNE(kTuneSick, {64, 160}, {63, 160}, {62, 320});
TUNE(kTuneMedicine, {72, 60}, {79, 90});
TUNE(kTuneClean, {72, 35}, {74, 35}, {76, 35}, {77, 35}, {79, 35}, {81, 35}, {83, 35}, {84, 90});
TUNE(kTuneScold, {60, 120}, {0, 50}, {60, 160});
TUNE(kTuneNo, {65, 80}, {60, 140});
TUNE(kTuneDeath, {72, 300}, {71, 300}, {69, 300}, {67, 300}, {65, 700});
TUNE(kTuneTap, {96, 14});
#undef TUNE
#define PLAY(kind, tune) playTune(SoundKind::kind, tune, sizeof(tune) / sizeof(tune[0]))

void loadSound() {
  Preferences p;
  p.begin("pet", true);
  sound.on = p.getBool("sound", true);
  sound.clicks = p.getBool("clicks", true);
  sound.actions = p.getBool("actions", true);
  sound.alerts = p.getBool("alerts", true);
  sound.volume = p.getUChar("volume", 60);
  sound.quietFrom = p.getUShort("quietFrom", sound.quietFrom);
  sound.quietTo = p.getUShort("quietTo", sound.quietTo);
  p.end();
}

void saveSound() {
  Preferences p;
  p.begin("pet", false);
  p.putBool("sound", sound.on);
  p.putBool("clicks", sound.clicks);
  p.putBool("actions", sound.actions);
  p.putBool("alerts", sound.alerts);
  p.putUChar("volume", sound.volume);
  p.putUShort("quietFrom", sound.quietFrom);
  p.putUShort("quietTo", sound.quietTo);
  p.end();
  if (audioReady) player.setVolume(sound.volume);
}

bool quietNow() {
  if (sound.quietFrom == sound.quietTo) return false;
  const time_t now = time(nullptr);
  tm t;
  gmtime_r(&now, &t);  // the clock keeps local time
  const int m = t.tm_hour * 60 + t.tm_min;
  return sound.quietFrom < sound.quietTo ? m >= sound.quietFrom && m < sound.quietTo
                                         : m >= sound.quietFrom || m < sound.quietTo;
}

// Plays a tune unless muted (all sound, or this kind) or, for alerts, in quiet hours. Locked and
// asleep, the codec is powered down: wake it for the chirp, wait for it, and power it down again.
void playTune(SoundKind kind, const AudioPlayer::Note *notes, size_t count) {
  if (!audioReady || !sound.on) return;
  if (kind == SoundKind::Click && !sound.clicks) return;
  if (kind == SoundKind::Action && !sound.actions) return;
  if (kind == SoundKind::Alert && (!sound.alerts || quietNow())) return;
  const bool wasDown = !player.isPoweredUp();
  if (wasDown && !player.powerUp()) return;
  player.playNotes(notes, count);
  if (wasDown) {
    const uint32_t start = millis();
    while (player.isPlaying() && millis() - start < 4000) delay(10);
    delay(30);
    player.powerDown();
  }
}

// One sound for what just happened (the most important first).
void soundFor(uint32_t events) {
  if (events & pet::kDied) PLAY(Alert, kTuneDeath);
  else if (events & pet::kHatched) PLAY(Alert, kTuneHatch);
  else if (events & pet::kEvolved) PLAY(Alert, kTuneEvolve);
  else if (events & pet::kGotSick) PLAY(Alert, kTuneSick);
  else if (events & pet::kCalled) PLAY(Alert, kTuneCall);
  else if (events & pet::kPooped) PLAY(Alert, kTunePoop);
}

String path(const char *name) {
  return storage::myDataDir() + "/" + name;
}

uint32_t nowMinute() {
  return static_cast<uint32_t>(time(nullptr) / 60);
}

bool clockValid() {
  return time(nullptr) > 1700000000;
}

// Our species (the P1's characters get our own names and art).
const char *const kSpeciesNames[pet::kSpeciesCount] = {"Egg",    "Dotlet", "Puffle", "Sprig",    "Lumpkin", "Lumo",
                                                       "Bramble", "Masko", "Chompy", "Wiggle", "Spike",  "Sir Moss"};
const char *speciesName(pet::Species s) {
  return s < pet::kSpeciesCount ? kSpeciesNames[s] : "?";
}
const char *stageName(pet::Stage s) {
  switch (s) {
    case pet::Stage::Egg: return "egg";
    case pet::Stage::Baby: return "baby";
    case pet::Stage::Child: return "child";
    case pet::Stage::Teen: return "teen";
    case pet::Stage::Adult: return "adult";
    default: return "dead";
  }
}

// ---------- saving ----------

constexpr uint32_t kMagic = 0x50455431;  // "PET1"

void save() {
  if (!sdReady) return;
  File f = SD_MMC.open(path("pet.part"), FILE_WRITE);
  if (!f) return;
  const uint32_t size = sizeof(state);
  f.write(reinterpret_cast<const uint8_t *>(&kMagic), 4);
  f.write(reinterpret_cast<const uint8_t *>(&size), 4);
  f.write(reinterpret_cast<const uint8_t *>(&state), size);
  f.close();
  SD_MMC.remove(path("pet.bin"));
  SD_MMC.rename(path("pet.part"), path("pet.bin"));
  lastSavedMinute = state.lastMinute;
}

bool load() {
  File f = SD_MMC.open(path("pet.bin"));
  if (!f) return false;
  uint32_t magic = 0, size = 0;
  f.read(reinterpret_cast<uint8_t *>(&magic), 4);
  f.read(reinterpret_cast<uint8_t *>(&size), 4);
  pet::State loaded;
  if (magic != kMagic || size != sizeof(loaded) || f.read(reinterpret_cast<uint8_t *>(&loaded), size) != size ||
      loaded.version != pet::kStateVersion) {
    return false;
  }
  state = loaded;
  return true;
}

const char *deathName(pet::Death d) {
  switch (d) {
    case pet::Death::Sickness: return "sickness";
    case pet::Death::OldAge: return "old age";
    case pet::Death::Neglect: return "neglect";
    case pet::Death::Starved: return "hunger";
    case pet::Death::Snacks: return "too many snacks";
    default: return "";
  }
}

// A line per pet that lived: generation, species, age, weight, cause, born, died (minutes).
void recordDeath() {
  if (!sdReady) return;
  File f = SD_MMC.open(path("history.tsv"), FILE_APPEND);
  if (!f) return;
  f.printf("%lu\t%d\t%d\t%d\t%s\t%lu\t%lu\n", static_cast<unsigned long>(state.generation), state.species, state.age,
           state.weight, deathName(state.death), static_cast<unsigned long>(state.bornMinute),
           static_cast<unsigned long>(state.lastMinute));
}

// ---------- time ----------

// Lives up to now (also while locked: called at each minute wake). Returns the events.
uint32_t advance() {
  if (!clockValid()) return 0;
  const uint32_t now = nowMinute();
  if (now + 5 < state.lastMinute) state.lastMinute = now;  // the clock was set back
  const pet::Death before = state.death;
  const uint32_t events = pet::catchUp(state, now);
  if (state.death != pet::Death::None && before == pet::Death::None) recordDeath();
  if (events & (pet::kHatched | pet::kEvolved | pet::kDied | pet::kGotSick | pet::kCalled | pet::kPooped)) save();
  else if (state.lastMinute - lastSavedMinute >= kSaveEveryMinutes) save();
  pendingEvents |= events;
  soundFor(events);
  if (events) notifyChanged();
  return events;
}

// ---------- drawing helpers ----------

// A 1-bit bitmap (MSB-first rows) scaled up by `scale`, optionally mirrored.
void drawScaled(Adafruit_GFX &gfx, const uint8_t *bmp, int w, int h, int16_t x, int16_t y, int scale,
                uint16_t color, bool mirror = false) {
  const int bytesPerRow = (w + 7) / 8;
  for (int row = 0; row < h; row++) {
    for (int col = 0; col < w; col++) {
      const int src = mirror ? w - 1 - col : col;
      if (pgm_read_byte(bmp + row * bytesPerRow + src / 8) & (0x80 >> (src % 8))) {
        gfx.fillRect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

void drawIcon(Adafruit_GFX &gfx, art::Icon icon, int16_t x, int16_t y, int scale = 1, uint16_t color = kBlack) {
  drawScaled(gfx, art::kIcons[icon], 16, 16, x, y, scale, color);
}

// The species index in the art (P1 order) for the engine's species.
int artIndex(pet::Species s) {
  return s >= pet::kBaby && s <= pet::kSecret ? s - pet::kBaby : 0;
}

void drawPet(Adafruit_GFX &gfx, art::Pose pose, int16_t x, int16_t y, int scale, bool mirror = false,
             uint16_t color = kBlack) {
  if (state.species == pet::kEgg) {
    drawScaled(gfx, art::kEgg[pose == art::kBob ? 1 : 0], art::kPetSize, art::kPetSize, x, y, scale, color);
    return;
  }
  if (pet::stage(state) == pet::Stage::Dead) {
    drawScaled(gfx, art::kAngel[0], art::kPetSize, art::kPetSize, x, y, scale, color);
    return;
  }
  drawScaled(gfx, art::kPets[artIndex(state.species)][pose], art::kPetSize, art::kPetSize, x, y, scale, color, mirror);
}

// The pose that tells how it is right now.
art::Pose moodPose() {
  if (state.asleep) return art::kBlink;
  if (state.sick) return art::kSad;
  if (state.hunger == 0 || state.happy == 0) return art::kSad;
  if (state.disciplineCall) return art::kAngry;
  return bobFrame ? art::kBob : art::kIdle;
}

void drawHearts(Adafruit_GFX &gfx, int16_t x, int16_t y, uint8_t full, int scale = 1) {
  for (int i = 0; i < 4; i++) {
    drawIcon(gfx, i < full ? art::kHeartIcon : art::kHeartEmptyIcon, x + i * 18 * scale, y, scale);
  }
}

// What it needs most, in a few words (lock screen, the bell).
String needs() {
  if (pet::stage(state) == pet::Stage::Dead) return "Passed away";
  if (state.species == pet::kEgg) return "Hatching soon";
  if (state.paused) return "Paused";
  if (state.sick) return "Sick! Medicine";
  if (state.hunger == 0) return "Hungry!";
  if (state.happy == 0) return "Wants to play";
  if (state.disciplineCall) return "Misbehaving!";
  if (state.lightsCall) return "Light still on";
  if (state.poops) return "Needs a clean";
  if (state.asleep) return "Sleeping";
  return "All good";
}

// ---------- screens ----------

void drawBars() {
  for (int i = 0; i < 8; i++) {
    const int16_t cx = (i % 4) * 50, cy = i < 4 ? 0 : kPlayBottom;
    const bool lit = i == kAttention && pet::calling(state);
    if (lit) epd.fillRect(cx + 1, cy + 1, 48, kBarH - 2, kBlack);
    drawIcon(epd, kBarIcons[i], cx + 9, cy + 4, 2, lit ? kWhite : kBlack);
  }
  for (int16_t x = 0; x < kW; x += 4) {  // dotted lines between the bars and the pet
    epd.drawPixel(x, kPlayTop, kBlack);
    epd.drawPixel(x, kPlayBottom - 1, kBlack);
  }
}

void drawHome() {
  epd.fillScreen(kWhite);
  if (pet::stage(state) == pet::Stage::Dead) {  // the whole screen: the angel, then a new egg
    drawPet(epd, art::kIdle, (kW - kPetPx) / 2, 12, kPetScale);
    epd.setFont(&FreeSansBold9pt7b);
    ui::drawCentered(epd, "Age " + String(state.age) + ", " + deathName(state.death), 140);
    epd.setFont(&FreeSans9pt7b);
    ui::drawCentered(epd, "Tap for a new egg", 172);
    return;
  }
  drawBars();
  const bool dark = !state.lightsOn;
  if (dark) epd.fillRect(0, kPlayTop + 1, kW, kPlayBottom - kPlayTop - 2, kBlack);
  const uint16_t ink = dark ? kWhite : kBlack;
  epd.setTextColor(ink);
  if (dark && !state.asleep) {
    epd.setFont(&FreeSans9pt7b);
    ui::drawCentered(epd, "Lights off", kPlayTop + 64);
  } else {
    drawPet(epd, moodPose(), petX, kPetY, kPetScale, false, ink);
  }
  // Poop bottom-right, up to 4 (2 x 2).
  for (int i = 0; i < state.poops && !dark; i++) {
    drawIcon(epd, bobFrame ? art::kPoop2Icon : art::kPoopIcon, kW - 68 + (i % 2) * 34, kPlayBottom - 36 - (i / 2) * 34, 2, ink);
  }
  if (state.asleep) drawIcon(epd, art::kZzzIcon, petX + kPetPx - 20, kPetY - 6, 2, ink);
  if (state.sick) drawIcon(epd, art::kSkullIcon, 6, kPlayTop + 8, 2, ink);
  if (state.paused) {
    epd.fillRect(0, kPlayBottom - 26, kW, 24, kBlack);
    epd.setTextColor(kWhite);
    epd.setFont(&FreeSansBold9pt7b);
    ui::drawCentered(epd, "Paused", kPlayBottom - 9);
  }
  epd.setTextColor(kBlack);
}

// Two big choices side by side (Food: meal/snack, Light: on/off).
void drawChoice(const char *title, art::Icon left, const char *leftLabel, art::Icon right, const char *rightLabel) {
  epd.fillScreen(kWhite);
  nav::draw(epd, title, nav::Icon::Back, nav::Icon::None);
  const int16_t top = nav::kHeight + 8, h = kW - top - 8;
  for (int i = 0; i < 2; i++) {
    const int16_t x = 6 + i * 97;
    epd.drawRoundRect(x, top, 91, h, 10, kBlack);
    epd.drawRoundRect(x + 1, top + 1, 89, h - 2, 9, kBlack);
    drawIcon(epd, i ? right : left, x + 21, top + 22, 3);
    epd.setFont(&FreeSansBold9pt7b);
    const char *label = i ? rightLabel : leftLabel;
    epd.setCursor(x + (91 - ui::textWidth(epd, label)) / 2, top + h - 22);
    epd.print(label);
  }
}

void drawMeter() {
  epd.fillScreen(kWhite);
  nav::draw(epd, "Meter", nav::Icon::Back, nav::Icon::None);
  epd.setFont(&FreeSansBold9pt7b);
  int16_t y = nav::kHeight + 22;
  epd.setCursor(10, y);
  epd.print("Age " + String(state.age));
  const String weight = String(state.weight) + " g";
  epd.setCursor(kW - 10 - ui::textWidth(epd, weight), y);
  epd.print(weight);
  y += 12;
  // Discipline bar.
  epd.setFont(&FreeSans9pt7b);
  epd.setCursor(10, y + 14);
  epd.print("Discipline");
  const int16_t barX = 98, barW = kW - barX - 10;
  epd.drawRect(barX, y + 2, barW, 14, kBlack);
  epd.fillRect(barX + 2, y + 4, (barW - 4) * pet::disciplinePercent(state) / 100, 10, kBlack);
  y += 26;
  epd.setCursor(10, y + 13);
  epd.print("Hunger");
  drawHearts(epd, kW - 10 - 4 * 18 + 2, y, state.hunger);
  y += 24;
  epd.setCursor(10, y + 13);
  epd.print("Happy");
  drawHearts(epd, kW - 10 - 4 * 18 + 2, y, state.happy);
  // Pause and sound: two buttons, dark when on.
  const int16_t by = kW - 42;
  const struct { bool on; const char *label; } buttons[] = {
      {state.paused, state.paused ? "Resume" : "Pause"}, {!sound.on, sound.on ? "Sound on" : "Muted"}};
  epd.setFont(&FreeSansBold9pt7b);
  for (int i = 0; i < 2; i++) {
    const int16_t x = 6 + i * 97;
    epd.fillRoundRect(x, by, 91, 34, 17, buttons[i].on ? kBlack : kWhite);
    epd.drawRoundRect(x, by, 91, 34, 17, kBlack);
    epd.setTextColor(buttons[i].on ? kWhite : kBlack);
    epd.setCursor(x + (91 - ui::textWidth(epd, buttons[i].label)) / 2, by + 23);
    epd.print(buttons[i].label);
  }
  epd.setTextColor(kBlack);
}

// The game menu: the five games and Surprise me, 2 x 3.
void drawGameMenu() {
  epd.fillScreen(kWhite);
  nav::draw(epd, "Games", nav::Icon::Back, nav::Icon::None);
  const int16_t top = nav::kHeight + 2, h = (kW - top) / 3;
  epd.setFont(&FreeSansBold9pt7b);
  for (int i = 0; i < 6; i++) {
    const int16_t x = (i % 2) * 100, y = top + (i / 2) * h;
    epd.drawRoundRect(x + 3, y + 3, 94, h - 6, 8, kBlack);
    const char *label = i < 5 ? kGameNames[i] : "Surprise";
    epd.setCursor(x + (100 - ui::textWidth(epd, label)) / 2, y + h / 2 + 6);
    epd.print(label);
  }
}

// Shells: a cup at one of three places, lifted (showing what's under it) or down.
constexpr int16_t kCupY = 150;
int16_t cupX(int i) { return 40 + i * 60; }
void drawCup(int i, bool lifted) {
  const int16_t cx = cupX(i), base = lifted ? kCupY - 34 : kCupY;
  for (int d = 0; d < 2; d++) {  // a cup: a trapezoid, bottom wider, 2 px outline
    epd.drawLine(cx - 16 + d, base - 34, cx - 22 + d, base, kBlack);
    epd.drawLine(cx + 16 - d, base - 34, cx + 22 - d, base, kBlack);
  }
  epd.fillRect(cx - 16, base - 36, 33, 4, kBlack);
  epd.fillRect(cx - 23, base - 1, 47, 4, kBlack);
}

// Memory: four quarters with an icon each; the lit one is dark.
constexpr int16_t kQuadTop = 70;
const art::Icon kQuadIcons[4] = {art::kHeartIcon, art::kBulbIcon, art::kSnackIcon, art::kDuckIcon};
void drawQuads(int lit) {
  const int16_t h = (kW - kQuadTop) / 2;
  for (int q = 0; q < 4; q++) {
    const int16_t x = (q % 2) * 100, y = kQuadTop + (q / 2) * h;
    if (q == lit) epd.fillRoundRect(x + 3, y + 3, 94, h - 6, 10, kBlack);
    else epd.drawRoundRect(x + 3, y + 3, 94, h - 6, 10, kBlack);
    drawIcon(epd, kQuadIcons[q], x + 34, y + (h - 32) / 2, 2, q == lit ? kWhite : kBlack);
  }
}

void drawGame() {
  epd.fillScreen(kWhite);
  String title = kGameNames[static_cast<int>(game.kind)];
  if (game.kind == GameKind::Memory) title = game.phase == Phase::Show ? "Watch..." : game.phase == Phase::Input ? "Your turn" : "Memory";
  nav::draw(epd, title, nav::Icon::Back, nav::Icon::None);
  // Score: a dot per round, filled when won.
  for (int i = 0; i < 5; i++) {
    const int16_t cx = kW / 2 - 40 + i * 20, cy = nav::kHeight + 12;
    if (i < game.round || (i == game.round && game.phase == Phase::Result)) {
      if (game.results & (1 << i)) epd.fillCircle(cx, cy, 6, kBlack);
      else epd.drawCircle(cx, cy, 6, kBlack);
    } else {
      epd.drawCircle(cx, cy, 3, kBlack);
    }
  }
  const bool result = game.phase == Phase::Result;
  const char *verdict = game.lastWon ? "Yes!" : "No...";
  switch (game.kind) {
    case GameKind::LeftRight: {
      const int16_t py = nav::kHeight + 26;
      if (!result) {
        drawPet(epd, art::kIdle, (kW - kPetPx) / 2, py, kPetScale);
        epd.setFont(&FreeSans9pt7b);
        ui::drawCentered(epd, "Which way will it look?", kW - 8);
      } else {  // it looked one way: the guessed one when the round was won
        const bool lookedLeft = game.lastWon ? game.guessedLeft : !game.guessedLeft;
        drawPet(epd, game.lastWon ? art::kHappy : art::kNo, (kW - kPetPx) / 2, py, kPetScale, !lookedLeft);
        epd.setFont(&FreeSansBold12pt7b);
        ui::drawCentered(epd, verdict, kW - 8);
      }
      break;
    }
    case GameKind::HigherLower: {
      epd.setFont(&FreeSansBold24pt7b);
      const String shown = result ? String(game.number) + "  " + String(game.nextNumber) : String(game.number);
      ui::drawCentered(epd, shown, 128);
      if (result) {
        epd.setFont(&FreeSansBold12pt7b);
        ui::drawCentered(epd, verdict, 180);
      } else {
        epd.setFont(&FreeSans9pt7b);
        ui::drawCentered(epd, "Is the next one", 84);
        epd.setFont(&FreeSansBold9pt7b);
        for (int i = 0; i < 2; i++) {
          const int16_t x = 8 + i * 96;
          epd.drawRoundRect(x, 152, 88, 40, 20, kBlack);
          epd.drawRoundRect(x + 1, 153, 86, 38, 19, kBlack);
          const char *label = i ? "Higher" : "Lower";
          epd.setCursor(x + (88 - ui::textWidth(epd, label)) / 2, 178);
          epd.print(label);
        }
      }
      if (result) {  // a little arrow between the two numbers
        epd.fillTriangle(kW / 2 - 6, 106, kW / 2 + 6, 112, kW / 2 - 6, 118, kBlack);
      }
      break;
    }
    case GameKind::Shells: {
      const bool showPet = game.phase == Phase::Show && game.swapIndex < 0;
      for (int i = 0; i < 3; i++) {
        const bool lifted = showPet ? i == game.petAt : result && (i == game.petAt || i == game.picked);
        if (lifted && i == game.petAt) {
          drawPet(epd, result && !game.lastWon ? art::kSad : art::kHappy, cupX(i) - 24, kCupY - 46, 1);
        }
        drawCup(i, lifted);
      }
      epd.setFont(&FreeSans9pt7b);
      if (game.phase == Phase::Show && game.swapIndex >= 0) {  // the swap: a curved arrow between two cups
        const int a = game.swaps[game.swapIndex][0], b = game.swaps[game.swapIndex][1];
        const int16_t x0 = cupX(min(a, b)), x1 = cupX(max(a, b)), top = 82;
        for (int d = 0; d < 3; d++) {
          epd.drawLine(x0, 108 - d, x0 + 10, top - d, kBlack);
          epd.drawLine(x0 + 10, top - d, x1 - 10, top - d, kBlack);
          epd.drawLine(x1 - 10, top - d, x1, 108 - d, kBlack);
        }
        epd.fillTriangle(x0 - 6, 100, x0 + 6, 100, x0, 112, kBlack);
        epd.fillTriangle(x1 - 6, 100, x1 + 6, 100, x1, 112, kBlack);
        ui::drawCentered(epd, String(game.swapIndex + 1) + " of " + String(game.swapCount), kW - 6);
      } else if (showPet) {
        ui::drawCentered(epd, "Watch the cups!", kW - 6);
      } else if (game.phase == Phase::Input) {
        ui::drawCentered(epd, "Where is it?", kW - 6);
      } else {
        epd.setFont(&FreeSansBold12pt7b);
        ui::drawCentered(epd, verdict, kW - 6);
      }
      break;
    }
    case GameKind::Memory:
      drawQuads(game.lit);
      if (result) {  // over the quarters: how it went
        epd.fillRoundRect(40, 115, 120, 40, 12, kWhite);
        epd.drawRoundRect(40, 115, 120, 40, 12, kBlack);
        epd.setFont(&FreeSansBold12pt7b);
        ui::drawCentered(epd, verdict, 143);
      }
      break;
    case GameKind::Clap: {
      drawPet(epd, result ? (game.lastWon ? art::kHappy : art::kSad) : game.listening ? art::kEat : art::kIdle,
              (kW - 48) / 2, nav::kHeight + 18, 1);
      epd.setFont(&FreeSansBold12pt7b);
      ui::drawCentered(epd, result ? verdict : game.listening ? "Clap it back!" : "Listen...", 136);
      // The rhythm as dots spaced by time: Dotty's beats, and under them your claps.
      const uint16_t span = max<uint16_t>(game.patternMs, 1);
      auto at = [&](uint16_t ms) { return static_cast<int16_t>(min<int>(20 + 160L * ms / span, kW - 8)); };
      for (int i = 0; i < game.target; i++) epd.fillCircle(at(game.beatAt[i]), 154, 6, kBlack);
      if (game.listening || result) {
        for (int i = 0; i < game.claps && i < 8; i++) {
          const int16_t x = at(game.clapAt[i]);
          epd.fillRect(x - 5, 172, 11, 11, kBlack);
        }
        epd.setFont(&FreeSans9pt7b);
        if (result && !game.lastWon) {
          ui::drawCentered(epd, game.claps == game.target ? "Close! Mind the gaps" : String(game.claps) + " claps, " + String(game.target) + " beats", kW - 2);
        }
      }
      break;
    }
    default:
      break;
  }
}

// The current frame of the running animation.
void drawAnim() {
  epd.fillScreen(kWhite);
  drawBars();
  const int16_t x = (kW - kPetPx) / 2 + 20;
  switch (anim.kind) {
    case Anim::Eat:
    case Anim::Snack: {
      const bool done = anim.frame >= anim.frames - 1;
      drawPet(epd, done ? art::kHappy : (anim.frame % 2 ? art::kIdle : art::kEat), x, kPetY, kPetScale);
      if (!done) {
        drawIcon(epd, anim.kind == Anim::Eat ? art::kMealIcon : art::kSnackIcon, 8, kPetY + 40, 2);
        // Bites: blank out the food from the right.
        const int bites = pet::info(state.species).bites;
        const int16_t gone = 32 * anim.frame / bites;
        if (gone) epd.fillRect(8 + 32 - gone, kPetY + 40, gone, 32, kWhite);
      }
      break;
    }
    case Anim::Refuse:
    case Anim::Nothing:
      drawPet(epd, art::kNo, x - 20 + (anim.frame % 2 ? 8 : -8), kPetY, kPetScale);
      break;
    case Anim::Medicine:
      drawPet(epd, anim.frame == anim.frames - 1 ? (state.sick ? art::kSad : art::kHappy) : art::kSad, x, kPetY, kPetScale);
      if (anim.frame < anim.frames - 1) drawIcon(epd, art::kSyringeIcon, 10 + anim.frame * 14, kPetY + 10, 2);
      break;
    case Anim::Clean: {
      drawPet(epd, art::kIdle, x - 20, kPetY, kPetScale);
      const int16_t dx = kW - 40 - anim.frame * 60;  // the duck sweeps right to left
      drawIcon(epd, art::kDuckIcon, dx, kPetY + 50, 2);
      for (int16_t i = dx + 32; i < kW; i += 6) epd.drawFastHLine(i, kPetY + 84, 3, kBlack);
      break;
    }
    case Anim::Scold:
      drawPet(epd, anim.frame == 0 ? art::kAngry : art::kSad, x - 20, kPetY, kPetScale);
      drawIcon(epd, art::kAttentionIcon, 14, kPetY + 10, 2);
      break;
    case Anim::NoScold:
      drawPet(epd, art::kNo, x - 20, kPetY, kPetScale);
      epd.setFont(&FreeSansBold18pt7b);
      epd.setCursor(20, kPetY + 50);
      epd.print("?");
      break;
    case Anim::GameEnd:
      drawPet(epd, game.wins >= 3 ? art::kHappy : art::kSad, x - 20, kPetY, kPetScale);
      epd.setFont(&FreeSansBold9pt7b);
      ui::drawCentered(epd, String(game.wins) + " of 5" + (game.wins >= 3 ? " - happy!" : ""), kPlayBottom - 6);
      break;
    case Anim::Hatch: {
      // The egg cracks, its top tips off and falls to the side, and out comes the baby.
      const int16_t cx = (kW - kPetPx) / 2;
      auto shell = [&](int i, int16_t x) {
        drawScaled(epd, art::kShell[i], art::kPetSize, art::kPetSize, x, kPetY, kPetScale, kBlack);
      };
      if (anim.frame == 0) {
        drawScaled(epd, art::kEgg[2], art::kPetSize, art::kPetSize, cx, kPetY, kPetScale, kBlack);
      } else if (anim.frame == 1) {
        shell(0, cx);
        shell(1, cx);
      } else if (anim.frame == 2) {
        shell(0, cx - 56);
        shell(2, cx + 58);
        drawPet(epd, art::kBlink, cx, kPetY, kPetScale);
      } else {
        drawPet(epd, art::kHappy, cx, kPetY, kPetScale);
        epd.setFont(&FreeSansBold9pt7b);
        ui::drawCentered(epd, "Hatched!", kPlayBottom - 6);
      }
      break;
    }
    case Anim::Evolve:
      drawPet(epd, anim.frame == 0 ? art::kBlink : art::kHappy, (kW - kPetPx) / 2, kPetY, kPetScale);
      epd.setFont(&FreeSansBold9pt7b);
      ui::drawCentered(epd, "Grew up!", kPlayBottom - 6);
      break;
    default:
      break;
  }
}

void drawApp() {
  switch (screen) {
    case Screen::Home: drawHome(); break;
    case Screen::Food: drawChoice("Food", art::kMealIcon, "Meal", art::kSnackIcon, "Snack"); break;
    case Screen::Light: drawChoice("Light", art::kBulbIcon, "On", art::kZzzIcon, "Off"); break;
    case Screen::GameMenu: drawGameMenu(); break;
    case Screen::Game: drawGame(); break;
    case Screen::Meter: drawMeter(); break;
    case Screen::Anim: drawAnim(); break;
  }
}

// ---------- lock screen: everything at a glance ----------

// The user's layout: top half = the pet (x3) | its data (clock, name, stage, age, weight, and
// what needs seeing as big icons); bottom half = the three meters as big rows.
bool drawPetLock(Adafruit_GFX &gfx, const LockScreenInfo &info) {
  advance();
  gfx.fillScreen(kWhite);
  gfx.setTextColor(kBlack);
  const bool alive = pet::stage(state) != pet::Stage::Dead && state.species != pet::kEgg;
  const art::Pose pose = state.asleep ? art::kBlink : (state.sick || state.hunger == 0 || state.happy == 0) ? art::kSad
                                                                                                          : art::kIdle;
  // Top left: the pet.
  drawPet(gfx, pose, 2, 2, 2);
  // Top right: clock + battery, then who it is.
  constexpr int16_t kX = 104;
  gfx.setFont(&FreeSansBold12pt7b);
  char clock[6];
  snprintf(clock, sizeof(clock), "%02d:%02d", info.time.tm_hour, info.time.tm_min);
  gfx.setCursor(kX, 20);
  gfx.print(info.timeValid ? clock : "--:--");
  ui::drawBattery(gfx, kW - 2 - 29, 6, info.batteryPercent, info.charging);
  gfx.setFont(&FreeSansBold9pt7b);
  gfx.setCursor(kX, 44);
  gfx.print(ui::fitText(gfx, speciesName(state.species), kW - kX - 2));
  gfx.setFont(&FreeSans9pt7b);
  gfx.setCursor(kX, 64);
  if (!alive) {
    gfx.print(state.species == pet::kEgg ? "Hatching soon" : "Passed away");
  } else {
    gfx.print("Age " + String(state.age) + "  " + String(state.weight) + " g");
  }
  if (alive) {
    // What needs seeing, as big icons (or "All good").
    int16_t x = kX - 2;
    auto add = [&](art::Icon icon) {
      if (x > kW - 32) return;
      drawIcon(gfx, icon, x, 70, 2);
      x += 32;
    };
    if (pet::calling(state)) add(art::kAttentionIcon);
    if (state.sick) add(art::kSkullIcon);
    if (state.poops) add(art::kPoopIcon);
    if (state.lightsCall) add(art::kBulbIcon);
    if (state.asleep) add(art::kZzzIcon);
    if (state.paused) add(art::kPauseIcon);
    if (x == kX - 2) {
      gfx.setFont(&FreeSansBold9pt7b);
      gfx.setCursor(kX, 92);
      gfx.print(state.hunger == 0 ? "Hungry!" : state.happy == 0 ? "Bored!" : "All good");
    }
    // Bottom half: hunger, happiness, discipline (the P1's 4-step gauge).
    const struct { art::Icon icon; uint8_t value; int16_t y; } rows[] = {
        {art::kFoodIcon, state.hunger, 104}, {art::kGameIcon, state.happy, 136}};
    for (const auto &r : rows) {
      drawIcon(gfx, r.icon, 2, r.y, 2);
      for (int i = 0; i < 4; i++) drawIcon(gfx, i < r.value ? art::kHeartIcon : art::kHeartEmptyIcon, 40 + i * 40, r.y, 2);
    }
    // Discipline: one continuous bar, as on the Meter screen.
    drawIcon(gfx, art::kDisciplineIcon, 2, 168, 2);
    constexpr int16_t kBarX = 42, kBarW = kW - kBarX - 4;
    gfx.drawRoundRect(kBarX, 172, kBarW, 24, 5, kBlack);
    gfx.drawRoundRect(kBarX + 1, 173, kBarW - 2, 22, 4, kBlack);
    const int16_t fill = (kBarW - 8) * pet::disciplinePercent(state) / 100;
    if (fill > 0) gfx.fillRoundRect(kBarX + 4, 176, fill, 16, 3, kBlack);
  } else {
    gfx.setFont(&FreeSans9pt7b);
    ui::drawCentered(gfx, "Unlock to look after it", 150);
  }
  return false;
}

// ---------- actions ----------

void startAnim(Anim kind, int frames) {
  anim = {kind, 0, frames, millis() + kAnimFrameMs, false};
  switch (kind) {  // (hatching and evolving sound with their events)
    case Anim::Eat:
    case Anim::Snack: PLAY(Action, kTuneEat); break;
    case Anim::Refuse:
    case Anim::Nothing:
    case Anim::NoScold: PLAY(Action, kTuneNo); break;
    case Anim::Medicine: PLAY(Action, kTuneMedicine); break;
    case Anim::Clean: PLAY(Action, kTuneClean); break;
    case Anim::Scold: PLAY(Action, kTuneScold); break;
    case Anim::GameEnd:
      if (game.wins >= 3) PLAY(Action, kTuneHappy);
      else PLAY(Action, kTuneLose);
      break;
    default: break;
  }
  screen = Screen::Anim;
  redraw = true;
}

void goHome() {
  screen = Screen::Home;
  redraw = true;
}

void act(Function f) {
  uint32_t events = 0;
  const bool alive = pet::stage(state) != pet::Stage::Dead && state.species != pet::kEgg;
  if (!alive && f != kMeter) return;
  switch (f) {
    case kFood: screen = Screen::Food; break;
    case kLight: screen = Screen::Light; break;
    case kGame:
      if (!pet::canPlay(state)) return startAnim(Anim::Refuse, 3);
      screen = Screen::GameMenu;
      break;
    case kMedicine:
      if (!pet::medicine(state, events)) return startAnim(Anim::Nothing, 3);
      save();
      return startAnim(Anim::Medicine, 3);
    case kClean:
      if (!pet::clean(state)) return startAnim(Anim::Nothing, 3);
      save();
      return startAnim(Anim::Clean, 3);
    case kMeter: screen = Screen::Meter; break;
    case kDiscipline:
      if (!pet::scold(state)) return startAnim(Anim::NoScold, 2);
      save();
      return startAnim(Anim::Scold, 2);
    case kAttention: break;
  }
  redraw = true;
}

void onFood(bool meal) {
  uint32_t events = 0;
  const bool ok = meal ? pet::feedMeal(state) : pet::feedSnack(state, events);
  pendingEvents |= events;
  if (!ok) return startAnim(Anim::Refuse, 3);
  save();
  startAnim(meal ? Anim::Eat : Anim::Snack, pet::info(state.species).bites + 1);
}

// ---------- the games ----------

using N = AudioPlayer::Note;
const N kQuadNotes[4][1] = {{{72, 320}}, {{76, 320}}, {{79, 320}}, {{84, 320}}};  // C E G C: one per quarter

void quadTone(int q) {
  playTune(SoundKind::Action, kQuadNotes[q], 1);
}

void stopListening() {
  if (game.listening) player.stopCapture();
  game.listening = false;
}

void endRound(bool won) {
  LOGI("pet", "%s round %d: %s", kGameNames[static_cast<int>(game.kind)], game.round + 1, won ? "won" : "lost");
  stopListening();
  game.lastWon = won;
  if (won) {
    game.wins++;
    game.results |= 1 << game.round;
    PLAY(Action, kTuneWin);
  } else {
    PLAY(Action, kTuneLose);
  }
  game.phase = Phase::Result;
  game.next = millis() + 1800;
  redraw = true;
}

// Sets up a round of the current game.
void beginRound() {
  const uint32_t now = millis();
  game.phase = Phase::Input;
  game.lit = -1;
  switch (game.kind) {
    case GameKind::LeftRight:
      break;
    case GameKind::HigherLower:
      game.number = hiloNumber;
      break;
    case GameKind::Shells: {
      static const int kSwaps[5] = {2, 3, 3, 4, 5};
      game.petAt = esp_random() % 3;
      game.swapCount = kSwaps[game.round];
      for (int i = 0; i < game.swapCount; i++) {
        const uint8_t a = esp_random() % 3;
        game.swaps[i][0] = a;
        game.swaps[i][1] = (a + 1 + esp_random() % 2) % 3;
      }
      game.swapIndex = -1;  // first, where it is
      game.phase = Phase::Show;
      game.next = now + 1800;
      break;
    }
    case GameKind::Memory:
      game.seqLen = 3 + game.round;  // 3 to 7
      for (int i = 0; i < game.seqLen; i++) game.seq[i] = esp_random() % 4;
      game.seqIndex = 0;
      game.inputIndex = 0;
      game.phase = Phase::Show;
      game.next = now + 1000;
      break;
    case GameKind::Clap: {
      // A pattern of 2 to 6 beats, 0.5 / 1 / 1.5 s apart, beeped now; then it listens.
      static const uint16_t kGaps[3] = {500, 1000, 1500};
      game.target = 2 + game.round;
      game.claps = 0;
      game.beatAt[0] = 0;
      for (int i = 1; i < game.target; i++) game.beatAt[i] = game.beatAt[i - 1] + kGaps[esp_random() % 3];
      game.patternMs = game.beatAt[game.target - 1];
      static AudioPlayer::Note tune[16];
      int n = 0;
      tune[n++] = {0, 900};  // a moment to get ready
      for (int i = 0; i < game.target; i++) {
        tune[n++] = {88, 90};
        if (i + 1 < game.target) tune[n++] = {0, static_cast<uint16_t>(game.beatAt[i + 1] - game.beatAt[i] - 90)};
      }
      playTune(SoundKind::Action, tune, n);
      game.phase = Phase::Show;
      game.next = now + 900 + game.patternMs + 700;
      break;
    }
    default:
      break;
  }
  redraw = true;
}

void startGame(GameKind kind) {
  game = GameState();
  game.kind = kind;
  screen = Screen::Game;
  beginRound();
}

// The claps match the pattern: as many, and each gap within 35 % (at least 0.25 s) of the beat's.
bool rhythmMatches() {
  if (game.claps != game.target) return false;
  for (int i = 1; i < game.target; i++) {
    const int want = game.beatAt[i] - game.beatAt[i - 1], got = game.clapAt[i] - game.clapAt[i - 1];
    if (abs(got - want) > max(250, want * 35 / 100)) return false;
  }
  return true;
}

// The microphone, while listening for claps: one 10 ms block per call. A clap is a sharp
// peak well above the room's own level (measured over the first 0.3 s), with a pause between
// counts so a clap's echo isn't counted twice. Each clap's time is kept, from the first one.
void listenForClaps() {
  constexpr size_t kBlock = 320;  // 10 ms at 32 kHz
  static int16_t samples[kBlock];
  static uint32_t firstClap = 0;
  const size_t got = player.capture(samples, kBlock);
  int peak = 0;
  for (size_t i = 0; i < got; i++) peak = max(peak, abs(static_cast<int>(samples[i])));
  const uint32_t now = millis();
  if (game.floorBlocks < 30) {  // the room's level first
    game.floorSum += peak;
    if (++game.floorBlocks == 30) game.threshold = max(4000, static_cast<int>(game.floorSum / 30) * 4);
    return;
  }
  if (!game.loud && peak > game.threshold && now - game.lastClap > 150) {
    game.loud = true;
    if (game.claps == 0) firstClap = now;
    if (game.claps < 8) game.clapAt[game.claps] = now - firstClap;
    game.claps++;
    game.lastClap = now;
    redraw = true;
  } else if (game.loud && peak < game.threshold / 2) {
    game.loud = false;
  }
  // Done 1.8 s after the last clap, with too many claps, or after 6 s of silence.
  if ((game.claps && now - game.lastClap > 1800) || game.claps > game.target + 2 ||
      (!game.claps && now - game.listenStart > 6000)) {
    endRound(rhythmMatches());
  }
}

// Timed steps: showing the cups, the sequence, the clap prompt, and moving on after a result.
void stepGame() {
  if (screen != Screen::Game) return;
  const uint32_t now = millis();
  if (game.kind == GameKind::Memory && game.phase == Phase::Input && game.lit >= 0 && now >= game.litUntil) {
    game.lit = -1;
    redraw = true;
  }
  if (game.phase == Phase::Input && game.kind == GameKind::Clap && game.listening) {
    listenForClaps();
    return;
  }
  if (now < game.next) return;
  if (game.phase == Phase::Result) {
    if (game.kind == GameKind::HigherLower) hiloNumber = game.nextNumber;
    if (++game.round >= 5) {
      pet::finishGame(state, game.wins);
      save();
      startAnim(Anim::GameEnd, 3);
    } else {
      beginRound();
    }
    return;
  }
  if (game.phase != Phase::Show) return;
  switch (game.kind) {
    case GameKind::Shells:
      if (game.swapIndex >= 0) {  // that swap happened: the pet moves with its cup
        const uint8_t a = game.swaps[game.swapIndex][0], b = game.swaps[game.swapIndex][1];
        if (game.petAt == a) game.petAt = b;
        else if (game.petAt == b) game.petAt = a;
      }
      if (++game.swapIndex >= game.swapCount) {
        game.phase = Phase::Input;
      } else {
        game.next = now + 1100;
      }
      redraw = true;
      break;
    case GameKind::Memory:
      if (game.lit < 0) {  // light the next one
        game.lit = game.seq[game.seqIndex];
        quadTone(game.lit);
        game.next = now + 750;
      } else {
        game.lit = -1;
        if (++game.seqIndex >= game.seqLen) game.phase = Phase::Input;
        game.next = now + 350;
      }
      redraw = true;
      break;
    case GameKind::Clap:
      if (player.startCapture(30, 0)) {
        game.listening = true;
        game.phase = Phase::Input;
        game.listenStart = now;
        game.floorSum = game.floorBlocks = 0;
        game.loud = false;
        game.lastClap = 0;
      } else {
        endRound(false);
      }
      redraw = true;
      break;
    default:
      break;
  }
}

void onGameTap(uint16_t x, uint16_t y) {
  if (game.phase != Phase::Input) return;
  switch (game.kind) {
    case GameKind::LeftRight:
      game.guessedLeft = x < kW / 2;
      endRound(pet::roundWon(state, esp_random() & 0xF));  // the P1's way: decided at random
      break;
    case GameKind::HigherLower: {
      if (y < 140) return;
      game.guessedHigher = x >= kW / 2;
      do {
        game.nextNumber = 1 + esp_random() % 9;
      } while (game.nextNumber == game.number);
      endRound(game.guessedHigher == (game.nextNumber > game.number));
      break;
    }
    case GameKind::Shells:
      if (y < 90) return;
      game.picked = x < 70 ? 0 : x < 130 ? 1 : 2;
      endRound(game.picked == game.petAt);
      break;
    case GameKind::Memory: {
      if (y < kQuadTop) return;
      const int q = (x < kW / 2 ? 0 : 1) + (y < kQuadTop + (kW - kQuadTop) / 2 ? 0 : 2);
      LOGI("pet", "memory tap %d (wanted %d, %d of %d)", q, game.seq[game.inputIndex], game.inputIndex + 1, game.seqLen);
      game.lit = q;
      game.litUntil = millis() + 300;
      quadTone(q);
      if (q != game.seq[game.inputIndex]) endRound(false);
      else if (++game.inputIndex >= game.seqLen) endRound(true);
      redraw = true;
      break;
    }
    default:
      break;
  }
}

void newEgg() {
  pet::newEgg(state, nowMinute());
  save();
  goHome();
  fullRedraw = true;
}

void onTap(uint16_t x, uint16_t y) {
  lastTouch = millis();
  PLAY(Click, kTuneTap);  // every tap clicks, like the original's buttons (actions then play their own)
  switch (screen) {
    case Screen::Home:
      if (pet::stage(state) == pet::Stage::Dead) {
        newEgg();  // a tap anywhere
        return;
      }
      if (y < kBarH) act(static_cast<Function>(x / 50));
      else if (y >= kPlayBottom) act(static_cast<Function>(4 + x / 50));
      else redraw = true;  // a touch wakes it up: it walks about for a bit
      break;
    case Screen::Food:
    case Screen::Light:
      if (y < nav::kHeight) {
        if (nav::hit(x, y) == -1) goHome();
        return;
      }
      if (screen == Screen::Food) {
        onFood(x < kW / 2);
      } else {
        pet::setLights(state, x < kW / 2);
        save();
        goHome();
      }
      break;
    case Screen::GameMenu:
      if (y < nav::kHeight) {
        if (nav::hit(x, y) == -1) goHome();
        return;
      }
      {
        const int16_t top = nav::kHeight + 2, h = (kW - top) / 3;
        const int cell = (x < 100 ? 0 : 1) + 2 * min(2, (y - top) / h);
        startGame(cell < 5 ? static_cast<GameKind>(cell) : static_cast<GameKind>(esp_random() % 5));
      }
      break;
    case Screen::Game:
      if (y < nav::kHeight) {
        if (nav::hit(x, y) == -1) {
          stopListening();
          goHome();
        }
        return;
      }
      onGameTap(x, y);
      break;
    case Screen::Meter:
      if (y < nav::kHeight && nav::hit(x, y) == -1) return goHome();
      if (y > kW - 46) {
        if (x < kW / 2) {
          pet::setPaused(state, !state.paused, nowMinute());
          save();
        } else {
          sound.on = !sound.on;
          saveSound();
          if (sound.on) PLAY(Click, kTuneTap);
        }
        redraw = true;
      }
      break;
    case Screen::Anim:
      break;
  }
}

void stepAnimations() {
  const uint32_t now = millis();
  if (screen == Screen::Anim && now >= anim.next) {
    if (++anim.frame >= anim.frames) {
      goHome();
    } else {
      anim.next = now + kAnimFrameMs;
      redraw = true;
    }
  }
  stepGame();
  // Walking about for a while after a touch.
  if (screen == Screen::Home && now - lastTouch < kIdleForMs && now >= nextIdleFrame && !state.asleep) {
    nextIdleFrame = now + kIdleFrameMs;
    bobFrame = !bobFrame;
    if (!bobFrame) petX = constrain(petX + static_cast<int>(esp_random() % 41) - 20, 6, kW - kPetPx - 40);
    redraw = true;
  }
}

// What happened since the screen last looked: animations for the big moments.
void showEvents() {
  const uint32_t events = pendingEvents;
  pendingEvents = 0;
  if (!events) return;
  if (events & pet::kDied) {
    goHome();
    fullRedraw = true;
  } else if (events & pet::kHatched) {
    startAnim(Anim::Hatch, 4);
    fullRedraw = true;
  } else if (events & pet::kEvolved) {
    startAnim(Anim::Evolve, 2);
    fullRedraw = true;
  } else if (screen == Screen::Home) {
    redraw = true;
  }
}

// ---------- app (BLE) ----------

void notifyChanged() {
  JsonDocument event;
  event["event"] = "pet.changed";
  ble::notify(event);
}

void registerCommands() {
  ble::on("pet.status", [](JsonObjectConst, JsonObject reply) {
    advance();
    reply["species"] = speciesName(state.species);
    reply["stage"] = stageName(pet::stage(state));
    reply["generation"] = state.generation;
    reply["age"] = state.age;
    reply["weight"] = state.weight;
    reply["hunger"] = state.hunger;
    reply["happy"] = state.happy;
    reply["discipline"] = pet::disciplinePercent(state);
    reply["careMistakes"] = state.careMistakes + state.adultMistakes;
    reply["disciplineMistakes"] = state.disciplineMistakes;
    reply["sick"] = state.sick;
    reply["poops"] = state.poops;
    reply["asleep"] = state.asleep;
    reply["lightsOn"] = state.lightsOn;
    reply["paused"] = state.paused;
    reply["calling"] = pet::calling(state);
    reply["needs"] = needs();
    if (state.death != pet::Death::None) reply["died"] = deathName(state.death);
    JsonObject s = reply["sound"].to<JsonObject>();
    s["on"] = sound.on;
    s["clicks"] = sound.clicks;
    s["actions"] = sound.actions;
    s["alerts"] = sound.alerts;
    s["volume"] = sound.volume;
    s["quietFrom"] = sound.quietFrom;
    s["quietTo"] = sound.quietTo;
  });
  ble::on("pet.pause", [](JsonObjectConst args, JsonObject) {
    pet::setPaused(state, args["on"] | !state.paused, nowMinute());
    save();
    redraw = true;
    notifyChanged();
  });
  // {on?, clicks?, actions?, alerts?, volume?, quietFrom?, quietTo?} (minutes of the day; equal
  // = no quiet hours, which only mute alerts)
  ble::on("pet.sound", [](JsonObjectConst args, JsonObject) {
    sound.on = args["on"] | sound.on;
    sound.clicks = args["clicks"] | sound.clicks;
    sound.actions = args["actions"] | sound.actions;
    sound.alerts = args["alerts"] | sound.alerts;
    sound.volume = constrain(args["volume"] | static_cast<int>(sound.volume), 0, 100);
    sound.quietFrom = constrain(args["quietFrom"] | static_cast<int>(sound.quietFrom), 0, 1439);
    sound.quietTo = constrain(args["quietTo"] | static_cast<int>(sound.quietTo), 0, 1439);
    saveSound();
    if (screen == Screen::Meter) redraw = true;
  });
  // The pets that lived before: {pets: [{generation, species, age, weight, cause, born, died}]}
  // (born/died: local time, minutes since 1970), newest first.
  ble::on("pet.history", [](JsonObjectConst, JsonObject reply) {
    JsonArray list = reply["pets"].to<JsonArray>();
    File f = SD_MMC.open(path("history.tsv"));
    std::vector<String> lines;
    while (f && f.available()) lines.push_back(f.readStringUntil('\n'));
    for (auto it = lines.rbegin(); it != lines.rend() && list.size() < 30; ++it) {
      char cause[24] = {};
      unsigned long gen = 0, born = 0, died = 0;
      int species = 0, age = 0, weight = 0;
      if (sscanf(it->c_str(), "%lu\t%d\t%d\t%d\t%23[^\t]\t%lu\t%lu", &gen, &species, &age, &weight, cause, &born,
                 &died) != 7) {
        continue;
      }
      JsonObject o = list.add<JsonObject>();
      o["generation"] = gen;
      o["species"] = speciesName(static_cast<pet::Species>(species));
      o["age"] = age;
      o["weight"] = weight;
      o["cause"] = cause;
      o["born"] = born;
      o["died"] = died;
    }
  });
  // A new egg; while it's alive only with {force: true} (the app asks first).
  ble::on("pet.newEgg", [](JsonObjectConst args, JsonObject reply) {
    if (pet::stage(state) != pet::Stage::Dead && !(args["force"] | false)) {
      reply["ok"] = false;
      reply["error"] = "it's still alive";
      return;
    }
    newEgg();
    notifyChanged();
  });
}

const shell::Picture kOffPictures[] = {
    {kSleepPanda, kSleepPandaWidth, kSleepPandaHeight},
};

}  // namespace

void setup() {
  shell::Config config;
  config.drawApp = drawApp;
  config.lockScreen = drawPetLock;
  config.lockedWakeSeconds = 60;
  config.onLockedWake = [] { return advance() != 0; };
  config.beforePowerOff = [] { save(); };
  config.sleepApp = [] {
    if (audioReady) player.powerDown();
  };
  config.wakeApp = [] {
    if (audioReady) player.powerUp();
  };
  config.offPictures = kOffPictures;
  config.offPictureCount = sizeof(kOffPictures) / sizeof(kOffPictures[0]);
  shell::begin(config);

  loadSound();
  audioReady = player.begin(32000);
  if (audioReady) player.setVolume(sound.volume);
  sdReady = storage::begin();
  if (sdReady) storage::makeDirs(storage::myDataDir());
  if (load()) {
    // It didn't live while Dotty was off or running another cartridge: carry on from now.
    state.lastMinute = nowMinute();
    LOGI("pet", "back: species %d, age %d, gen %lu", state.species, state.age, static_cast<unsigned long>(state.generation));
  } else {
    pet::newEgg(state, nowMinute());
    save();
    LOGI("pet", "a new egg");
  }
  registerCommands();
  lastTouch = millis();
  shell::showApp();
}

void loop() {
  shell::Input input;
  const bool unlocked = shell::update(input);
  if (!fastForward) advance();
  if (!unlocked) return;

  // Developer aids (serial): F fast-forward (~20 minutes a second), E evolve now, and the
  // eight functions: q w e r (top bar), z x c v (bottom bar).
  if (input.key == 'F') {
    fastForward = !fastForward;
    if (!fastForward) {
      state.lastMinute = nowMinute();
      save();
    }
    LOGI("pet", "fast-forward %s", fastForward ? "on" : "off");
  }
  if (input.key == 'E') state.toEvolve = 1;
  const bool menuWasOpen = screen == Screen::GameMenu;
  if (const char *k = input.key ? strchr("qwerzxcv", input.key) : nullptr) {
    lastTouch = millis();
    if (screen == Screen::Home) act(static_cast<Function>(k - "qwerzxcv"));
  }
  static uint32_t lastFast = 0;
  if (fastForward && millis() - lastFast >= 50) {
    lastFast = millis();
    uint32_t events = 0;
    state.lastMinute++;
    pet::step(state, state.lastMinute % 1440, events);
    pendingEvents |= events;
    if (events) redraw = true;
  }

  if (input.gesture == Touch::Gesture::Tap) onTap(shell::touch.x(), shell::touch.y());
  if (input.boot && screen != Screen::Home && screen != Screen::Anim) {
    stopListening();
    goHome();
  }
  // Developer aids in the games: a b c d e f pick a menu cell, C counts a clap.
  if (menuWasOpen && screen == Screen::GameMenu && input.key >= 'a' && input.key <= 'f') {
    const int cell = input.key - 'a';
    startGame(cell < 5 ? static_cast<GameKind>(cell) : static_cast<GameKind>(esp_random() % 5));
  }
  if (screen == Screen::Game && game.listening && input.key == 'C' && game.claps < 8) {  // a clap
    static uint32_t first = 0;
    if (game.claps == 0) first = millis();
    game.clapAt[game.claps++] = millis() - first;
    game.lastClap = millis();
    redraw = true;
  }
  stepAnimations();
  if (screen == Screen::Home || screen == Screen::Anim) showEvents();

  if ((redraw || fullRedraw) && !epd.isBusy()) {
    const bool full = fullRedraw;
    redraw = fullRedraw = false;
    drawApp();
    shell::refresh(full);
  }
  delay(10);
}
