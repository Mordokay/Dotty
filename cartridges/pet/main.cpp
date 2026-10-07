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
#include <Fonts/FreeSansBold9pt7b.h>
#include <SD_MMC.h>

#include "cartridge.h"
#include "core_ble.h"
#include "images/sleep_panda.h"
#include "images/sleep_portrait.h"
#include "images/sprites.h"
#include "log.h"
#include "nav_bar.h"
#include "pet_engine.h"
#include "shell.h"
#include "storage.h"
#include "ui.h"

DOTTY_CARTRIDGE("pet", "Pet", "0.1.0");

namespace {

using shell::epd;
constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

// Home layout: icon bars top and bottom, the pet between.
constexpr int16_t kBarH = 40;
constexpr int16_t kPlayTop = kBarH, kPlayBottom = kW - kBarH;
constexpr int kPetScale = 3;  // 32 px art → 96 px
constexpr int16_t kPetPx = art::kPetSize * kPetScale;
constexpr int16_t kPetY = kPlayTop + (kPlayBottom - kPlayTop - kPetPx) / 2;
constexpr uint32_t kIdleFrameMs = 1500;    // walking about after a touch…
constexpr uint32_t kIdleForMs = 20000;     // …for this long, then it holds still (panel wear)
constexpr uint32_t kAnimFrameMs = 700;     // animation frames (a partial refresh is ~0.6 s)
constexpr uint32_t kSaveEveryMinutes = 10;

enum class Screen { Home, Food, Light, Game, Meter, Anim };
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

// The game: 5 rounds of left or right.
struct GameState {
  int round = 0, wins = 0;
  uint8_t results = 0;  // bit per round: won
  bool waiting = true;  // for the player's guess
  bool lastWon = false, guessedLeft = false;
} game;

String path(const char *name) {
  return storage::myDataDir() + "/" + name;
}

uint32_t nowMinute() {
  return static_cast<uint32_t>(time(nullptr) / 60);
}

bool clockValid() {
  return time(nullptr) > 1700000000;
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
  drawBars();
  const bool dark = !state.lightsOn;
  if (dark) epd.fillRect(0, kPlayTop + 1, kW, kPlayBottom - kPlayTop - 2, kBlack);
  const uint16_t ink = dark ? kWhite : kBlack;
  epd.setTextColor(ink);
  if (pet::stage(state) == pet::Stage::Dead) {
    drawPet(epd, art::kIdle, (kW - 64) / 2, kPlayTop + 4, 2, false, ink);
    epd.setFont(&FreeSansBold9pt7b);
    ui::drawCentered(epd, "Age " + String(state.age) + ", " + deathName(state.death), kPlayTop + 88);
    epd.setFont(&FreeSans9pt7b);
    ui::drawCentered(epd, "Tap for a new egg", kPlayTop + 110);
    epd.setTextColor(kBlack);
    return;
  }
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
  // Pause button.
  const int16_t by = kW - 42;
  epd.fillRoundRect(40, by, kW - 80, 34, 17, state.paused ? kBlack : kWhite);
  epd.drawRoundRect(40, by, kW - 80, 34, 17, kBlack);
  epd.setTextColor(state.paused ? kWhite : kBlack);
  epd.setFont(&FreeSansBold9pt7b);
  ui::drawCentered(epd, state.paused ? "Resume" : "Pause", by + 23);
  epd.setTextColor(kBlack);
}

void drawGame() {
  epd.fillScreen(kWhite);
  nav::draw(epd, "Left or right?", nav::Icon::Back, nav::Icon::None);
  // Score: a dot per round, filled when won.
  for (int i = 0; i < 5; i++) {
    const int16_t cx = kW / 2 - 40 + i * 20, cy = nav::kHeight + 14;
    if (i < game.round) {  // played: filled when won
      if (game.results & (1 << i)) epd.fillCircle(cx, cy, 6, kBlack);
      else epd.drawCircle(cx, cy, 6, kBlack);
    } else {
      epd.drawCircle(cx, cy, 3, kBlack);
    }
  }
  const int16_t py = nav::kHeight + 28;
  if (game.waiting) {
    drawPet(epd, art::kIdle, (kW - kPetPx) / 2, py, kPetScale);
    epd.setFont(&FreeSans9pt7b);
    ui::drawCentered(epd, "Tap left or right", kW - 8);
  } else {
    // It looked one way: the same as the guess when the round was won.
    const bool lookedLeft = game.lastWon ? game.guessedLeft : !game.guessedLeft;
    drawPet(epd, game.lastWon ? art::kHappy : art::kNo, (kW - kPetPx) / 2, py, kPetScale, !lookedLeft);
    epd.setFont(&FreeSansBold12pt7b);
    ui::drawCentered(epd, game.lastWon ? "Yes!" : "No...", kW - 8);
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
    case Anim::Evolve:
    case Anim::Hatch:
      drawPet(epd, anim.frame == 0 ? art::kBlink : art::kHappy, (kW - kPetPx) / 2, kPetY, kPetScale);
      epd.setFont(&FreeSansBold9pt7b);
      ui::drawCentered(epd, anim.kind == Anim::Hatch ? "Hatched!" : "Grew up!", kPlayBottom - 6);
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
    case Screen::Game: drawGame(); break;
    case Screen::Meter: drawMeter(); break;
    case Screen::Anim: drawAnim(); break;
  }
}

// ---------- lock screen: everything at a glance ----------

bool drawPetLock(Adafruit_GFX &gfx, const LockScreenInfo &info) {
  advance();
  gfx.fillScreen(kWhite);
  gfx.setTextColor(kBlack);
  // Clock and battery.
  gfx.setFont(&FreeSansBold18pt7b);
  char clock[6];
  snprintf(clock, sizeof(clock), "%02d:%02d", info.time.tm_hour, info.time.tm_min);
  gfx.setCursor(8, 34);
  gfx.print(info.timeValid ? clock : "--:--");
  ui::drawBattery(gfx, kW - 8 - 29, 12, info.batteryPercent, info.charging);
  // The pet (x2) and its meters.
  const art::Pose pose = state.asleep ? art::kBlink : (state.sick || state.hunger == 0 || state.happy == 0) ? art::kSad
                                                                                                          : art::kIdle;
  drawPet(gfx, pose, 6, 50, 2);
  if (pet::stage(state) != pet::Stage::Dead && state.species != pet::kEgg) {
    drawIcon(gfx, art::kFoodIcon, 78, 52);
    for (int i = 0; i < 4; i++) drawIcon(gfx, i < state.hunger ? art::kHeartIcon : art::kHeartEmptyIcon, 98 + i * 24, 52);
    drawIcon(gfx, art::kGameIcon, 78, 76);
    for (int i = 0; i < 4; i++) drawIcon(gfx, i < state.happy ? art::kHeartIcon : art::kHeartEmptyIcon, 98 + i * 24, 76);
    // What else is going on: a row of icons.
    int16_t x = 78;
    auto add = [&](art::Icon icon) {
      if (x > kW - 18) return;
      drawIcon(gfx, icon, x, 100);
      x += 20;
    };
    for (int i = 0; i < state.poops; i++) add(art::kPoopIcon);
    if (state.sick) add(art::kSkullIcon);
    if (state.lightsCall) add(art::kBulbIcon);
    if (pet::calling(state)) add(art::kAttentionIcon);
    if (state.asleep) add(art::kZzzIcon);
    if (state.paused) add(art::kPauseIcon);
  }
  // What it needs, in words; dark when it's calling for you.
  const String text = needs();
  // Dark when something needs doing (anything but "All good", "Sleeping" and "Paused").
  const bool urgent = pet::stage(state) != pet::Stage::Dead && state.species != pet::kEgg && !state.paused &&
                      (pet::calling(state) || state.sick || state.hunger == 0 || state.happy == 0 || state.poops);
  if (urgent) gfx.fillRoundRect(6, 128, kW - 12, 34, 8, kBlack);
  gfx.setTextColor(urgent ? kWhite : kBlack);
  gfx.setFont(&FreeSansBold12pt7b);
  ui::drawCentered(gfx, text, 153);
  gfx.setTextColor(kBlack);
  gfx.setFont(&FreeSans9pt7b);
  const String line = state.species == pet::kEgg || pet::stage(state) == pet::Stage::Dead
                          ? String("Unlock to look after it")
                          : "Age " + String(state.age) + "  -  " + String(state.weight) + " g";
  ui::drawCentered(gfx, line, 188);
  return false;
}

// ---------- actions ----------

void startAnim(Anim kind, int frames) {
  anim = {kind, 0, frames, millis() + kAnimFrameMs, false};
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
      game = GameState();
      screen = Screen::Game;
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

void onGameTap(bool left) {
  if (!game.waiting) return;
  game.guessedLeft = left;
  game.lastWon = pet::roundWon(state, esp_random() & 0xF);
  if (game.lastWon) {
    game.wins++;
    game.results |= 1 << game.round;
  }
  game.round++;
  game.waiting = false;
  anim.next = millis() + 1500;  // show the result, then the next round
  redraw = true;
}

void newEgg() {
  pet::newEgg(state, nowMinute());
  save();
  goHome();
  fullRedraw = true;
}

void onTap(uint16_t x, uint16_t y) {
  lastTouch = millis();
  switch (screen) {
    case Screen::Home:
      if (pet::stage(state) == pet::Stage::Dead) {
        if (y > kPlayTop && y < kPlayBottom) newEgg();
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
    case Screen::Game:
      if (y < nav::kHeight && nav::hit(x, y) == -1) return goHome();
      onGameTap(x < kW / 2);
      break;
    case Screen::Meter:
      if (y < nav::kHeight && nav::hit(x, y) == -1) return goHome();
      if (y > kW - 46) {
        pet::setPaused(state, !state.paused, nowMinute());
        save();
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
  if (screen == Screen::Game && !game.waiting && now >= anim.next) {
    if (game.round >= 5) {
      pet::finishGame(state, game.wins);
      save();
      startAnim(Anim::GameEnd, 3);
    } else {
      game.waiting = true;
      redraw = true;
    }
  }
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
    startAnim(Anim::Hatch, 2);
    fullRedraw = true;
  } else if (events & pet::kEvolved) {
    startAnim(Anim::Evolve, 2);
    fullRedraw = true;
  } else if (screen == Screen::Home) {
    redraw = true;
  }
}

const shell::Picture kOffPictures[] = {
    {kSleepPortrait, kSleepPortraitWidth, kSleepPortraitHeight},
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
  config.offPictures = kOffPictures;
  config.offPictureCount = sizeof(kOffPictures) / sizeof(kOffPictures[0]);
  shell::begin(config);

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
  if (input.boot && screen != Screen::Home && screen != Screen::Anim) goHome();
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
