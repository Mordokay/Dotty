#include "touch_calibration.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include <algorithm>
#include <vector>

#include <SD_MMC.h>

#include "keyboard.h"
#include "log.h"
#include "ui.h"

namespace calibrate {
namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

// Crosses as far out as the chip can tell positions apart, with room for an unsteady tap: its
// x readings clamp in the outer ~27 px on the left and ~30 px on the right, and a tap that reads
// at the clamp teaches nothing (with crosses at 34 and 166, only the right column's taps that
// fell short survived, which bent the fit). 3 rows under the instructions (y doesn't clamp).
constexpr int16_t kTargetX[] = {36, 67, 98, 129, 160}, kTargetY[] = {66, 126, 186};
constexpr int kTargets = 15;
constexpr int16_t kMaxMiss = 35;  // a tap further than this from every cross doesn't count
constexpr int16_t kButtonsTop = 150;
constexpr int16_t kSkipW = 70, kSkipH = 40;  // the welcome flow's "Skip", top right

struct Sample {
  int16_t targetX, targetY;  // the cross
  uint16_t rawX, rawY;       // the chip's reading
  uint16_t x, y;             // where it landed with the calibration of its round
};

bool open_ = false;
bool onboarding = false;
bool showingResult = false;
// After Done: a typing test on the real keyboard (a few words that reach every edge column).
const char *const kWords[] = {"dotty", "quiz", "pumpkin"};
constexpr int kWordCount = 3;
bool typing = false;
int word = 0;      // kWordCount = finished: the summary
String typed;
int slips = 0;     // letters typed that didn't match (each needs a delete)
bool slipped = false;  // the text no longer matches the word
bool dirty = false;
int roundNo = 1;
std::vector<Sample> all;    // every round's taps (the fit uses them all)
std::vector<Sample> taps;   // this round's
std::vector<float> errors;  // each finished round: the average distance from its crosses (log)
std::vector<int> progress;  // each finished round: how calibrated, 0-100 % (see quality())
Touch::Calibration before;  // to put back if it's closed without saving

int16_t targetX(int i) { return kTargetX[i % 5]; }
int16_t targetY(int i) { return kTargetY[i / 5]; }

bool clamped(const Sample &s) {
  return s.rawX == 0 || s.rawX >= Touch::kRawMax || s.rawY == 0 || s.rawY >= Touch::kRawMax;
}

// The model's terms for a reading: 1, u, v, u v, u^2, v^2 (see Touch::Calibration).
void terms(const Sample &s, double *t) {
  const double u = (s.rawX - 100.0) / 100, v = (s.rawY - 100.0) / 100;
  t[0] = 1, t[1] = u, t[2] = v, t[3] = u * v, t[4] = u * u, t[5] = v * v;
}

// Least squares with the first k terms (3: a tilted plane, 6: the quadratic), both axes, every
// tap that isn't at the chip's clamp. Gaussian elimination on the normal equations.
bool fit(int k, Touch::Calibration &cal) {
  double a[Touch::kTerms][Touch::kTerms] = {}, bx[Touch::kTerms] = {}, by[Touch::kTerms] = {};
  int n = 0;
  for (const Sample &s : all) {
    if (clamped(s)) continue;
    double t[Touch::kTerms];
    terms(s, t);
    for (int i = 0; i < k; i++) {
      for (int j = 0; j < k; j++) a[i][j] += t[i] * t[j];
      bx[i] += t[i] * s.targetX;
      by[i] += t[i] * s.targetY;
    }
    n++;
  }
  if (n < 2 * k) return false;
  for (int col = 0; col < k; col++) {
    int pivot = col;
    for (int r = col + 1; r < k; r++) {
      if (fabs(a[r][col]) > fabs(a[pivot][col])) pivot = r;
    }
    if (fabs(a[pivot][col]) < 1e-9) return false;
    std::swap(a[col], a[pivot]);
    std::swap(bx[col], bx[pivot]);
    std::swap(by[col], by[pivot]);
    for (int r = 0; r < k; r++) {
      if (r == col) continue;
      const double f = a[r][col] / a[col][col];
      for (int c = col; c < k; c++) a[r][c] -= f * a[col][c];
      bx[r] -= f * bx[col];
      by[r] -= f * by[col];
    }
  }
  Touch::Calibration out;
  for (int i = 0; i < Touch::kTerms; i++) {
    out.x[i] = i < k ? bx[i] / a[i][i] : 0;
    out.y[i] = i < k ? by[i] / a[i][i] : 0;
  }
  cal = out;
  return true;
}

// How sure the correction is right: every tap so far through `cal`, averaged per cross, the
// root mean square of those averages (made up for the k terms the fit spent on these same
// crosses). A fingertip's own scatter (~4 px a tap) averages out as rounds add taps, so the
// bar fills round by round; what's left at the end is what the model can't fix (~1.4 px for the
// quadratic). Shown as a bar: 100 % at 2.5 px or less, 0 % at 7. (The straight-line fit with a
// column next to the clamp stayed ~3.3 px off after 31 rounds: 81 %.)
int quality(const Touch::Calibration &cal, int k) {
  struct Sum {
    int16_t x, y;
    double dx = 0, dy = 0;
    int n = 0;
  };
  std::vector<Sum> crosses;
  for (const Sample &s : all) {
    if (clamped(s)) continue;
    auto it = std::find_if(crosses.begin(), crosses.end(), [&](const Sum &c) { return c.x == s.targetX && c.y == s.targetY; });
    if (it == crosses.end()) {
      crosses.push_back({s.targetX, s.targetY});
      it = crosses.end() - 1;
    }
    float x, y;
    Touch::toScreen(cal, s.rawX, s.rawY, x, y);
    it->dx += x - s.targetX, it->dy += y - s.targetY, it->n++;
  }
  const int m = crosses.size();
  if (m <= k) return 0;
  double means = 0;
  for (const Sum &c : crosses) means += (c.dx * c.dx + c.dy * c.dy) / (c.n * c.n);
  const double off = sqrt(means / (m - k));
  return constrain(static_cast<int>(lround((7 - off) / 4.5 * 100)), 0, 100);
}

// Every calibration tap goes to the card too (/logs/touch.tsv), so better defaults can be fitted
// later from all of them (the log's RAM ring keeps only the last few rounds).
void logTap(const Sample &s) {
  const bool fresh = !SD_MMC.exists("/logs/touch.tsv");
  if (fresh) SD_MMC.mkdir("/logs");
  File f = SD_MMC.open("/logs/touch.tsv", FILE_APPEND);
  if (!f) return;
  if (fresh) f.println("epoch\tround\tcrossX\tcrossY\trawX\trawY\tlandedX\tlandedY");
  f.printf("%lu\t%d\t%d\t%d\t%u\t%u\t%u\t%u\n", static_cast<unsigned long>(time(nullptr)), roundNo, s.targetX,
           s.targetY, s.rawX, s.rawY, s.x, s.y);
  f.close();
}

void finishRound() {
  float total = 0;
  for (const Sample &s : taps) total += hypotf(s.x - s.targetX, s.y - s.targetY);
  errors.push_back(total / taps.size());
  // A tilted plane while there are only a tap or two per cross, then the quadratic.
  const int k = roundNo < 3 ? 3 : Touch::kTerms;
  Touch::Calibration cal = shell::touch.calibration();
  if (fit(k, cal)) shell::touch.setCalibration(cal, false);  // the next round uses it already
  progress.push_back(quality(shell::touch.calibration(), k));
  LOGI("touch", "calibration round %d: %.1f px off, %d %% calibrated (%d terms)", roundNo, errors.back(), progress.back(), k);
  showingResult = true;
}

void close(bool save) {
  if (save) shell::touch.setCalibration(shell::touch.calibration(), true);
  else shell::touch.setCalibration(before, false);
  open_ = false;
}

void drawCross(int16_t x, int16_t y) {
  epd.drawFastHLine(x - 7, y, 15, kBlack);
  epd.drawFastVLine(x, y - 7, 15, kBlack);
}

void drawButton(int16_t x, int16_t w, const char *label, bool dark) {
  if (dark) epd.fillRoundRect(x, kButtonsTop + 6, w, 36, 8, kBlack);
  else epd.drawRoundRect(x, kButtonsTop + 6, w, 36, 8, kBlack);
  epd.setFont(&FreeSansBold9pt7b);
  epd.setTextColor(dark ? kWhite : kBlack);
  epd.setCursor(x + (w - ui::textWidth(epd, label)) / 2, kButtonsTop + 30);
  epd.print(label);
  epd.setTextColor(kBlack);
}

void drawRound() {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  epd.setFont(&FreeSansBold9pt7b);
  const String title = onboarding && roundNo == 1 ? "First, my touch" : "Touch: round " + String(roundNo);
  if (onboarding) {  // skippable: the built-in calibration stays (touch.h's defaults)
    epd.setCursor(8, 20);
    epd.print(title);
    epd.setFont(&FreeSans9pt7b);
    const int16_t w = ui::textWidth(epd, "Skip");
    epd.setCursor(kW - 8 - w, 20);
    epd.print("Skip");
    epd.drawFastHLine(kW - 8 - w, 23, w, kBlack);
  } else {
    ui::drawCentered(epd, title, 20);
  }
  epd.setFont(&FreeSans9pt7b);
  ui::drawCentered(epd, "Tap each cross " + String(taps.size()) + "/" + String(kTargets), 44);
  for (int i = 0; i < kTargets; i++) drawCross(targetX(i), targetY(i));
  for (const Sample &s : taps) epd.fillCircle(s.x, s.y, 3, kBlack);
}

void drawResult() {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  const int now = progress.back();
  epd.setFont(&FreeSansBold9pt7b);
  ui::drawCentered(epd, "Calibration", 26);
  // A bar that fills up round by round.
  constexpr int16_t kBarX = 16, kBarY = 44, kBarW = kW - 32, kBarH = 22;
  epd.drawRoundRect(kBarX, kBarY, kBarW, kBarH, 6, kBlack);
  const int16_t filled = (kBarW - 6) * now / 100;
  if (filled > 0) epd.fillRoundRect(kBarX + 3, kBarY + 3, filled, kBarH - 6, 4, kBlack);
  ui::drawCentered(epd, String(now) + "%", 92);
  epd.setFont(&FreeSans9pt7b);
  String line = now >= 100 ? "All done!" : "Another round fills it up";
  if (now < 100 && progress.size() > 1 && now > progress[progress.size() - 2]) {
    line = "Up from " + String(progress[progress.size() - 2]) + "%";
  }
  ui::drawCentered(epd, line, 124);
  drawButton(8, 88, "Again", now < 100);  // the suggested one is dark
  drawButton(104, 88, "Done", now >= 100);
}

// ---------- the typing test ----------

void drawTyping() {
  epd.fillScreen(kWhite);
  epd.setTextColor(kBlack);
  if (word >= kWordCount) {  // the summary
    epd.setFont(&FreeSansBold9pt7b);
    ui::drawCentered(epd, slips == 0 ? "Perfect typing!" : slips <= 2 ? "Touch is ready!" : "A few slips", 40);
    epd.setFont(&FreeSans9pt7b);
    ui::drawCentered(epd, String(kWordCount) + " words, " + (slips == 0 ? String("no slips") : String(slips) + (slips == 1 ? " slip" : " slips")), 72);
    if (slips > 2) ui::drawCentered(epd, "Calibrating again may help", 100);
    drawButton(8, 88, "Again", slips > 2);
    drawButton(104, 88, "Done", slips <= 2);
    return;
  }
  epd.setFont(&FreeSansBold9pt7b);
  epd.setCursor(8, 22);
  epd.print("Type: " + String(kWords[word]));
  epd.setFont(&FreeSans9pt7b);
  const int16_t w = ui::textWidth(epd, "Skip");
  epd.setCursor(kW - 8 - w, 22);
  epd.print("Skip");
  epd.drawFastHLine(kW - 8 - w, 25, w, kBlack);
  // What's typed, the cursor after it; letters that went wrong are underlined.
  epd.drawRoundRect(4, 34, kW - 8, keyboard::kTop - 38, 5, kBlack);
  int16_t x = 12;
  const String target = kWords[word];
  for (size_t i = 0; i < typed.length(); i++) {
    const String c = typed.substring(i, i + 1);
    epd.setCursor(x, 60);
    epd.print(c);
    const int16_t cw = ui::textWidth(epd, c == " " ? String("n") : c);
    if (i >= target.length() || typed[i] != target[i]) epd.fillRect(x, 64, max<int16_t>(cw, 6), 2, kBlack);
    x += cw + 1;
  }
  epd.fillRect(x + 1, 46, 2, 17, kBlack);
  keyboard::draw();
}

void loopTyping(const shell::Input &input) {
  const uint16_t x = shell::touch.x(), y = shell::touch.y();
  const bool tap = input.gesture == Touch::Gesture::Tap;
  if (word >= kWordCount) {  // the summary: Again (calibrate again) / Done
    if (tap && y >= kButtonsTop) {
      if (x >= kW / 2) {
        open_ = false;
      } else {
        typing = false;
        showingResult = false;
        taps.clear();
        roundNo++;
        dirty = true;
      }
    }
    return;
  }
  if (tap && y < kSkipH && x >= kW - kSkipW) {
    open_ = false;
    return;
  }
  keyboard::Result result = keyboard::key(input.key, typed, 20);  // developer aid: serial letters
  if (tap) result = keyboard::tap(x, y, typed, 20);
  if (result == keyboard::Result::None) return;
  const String target = kWords[word];
  if (result == keyboard::Result::Typed) {
    const bool matches = target.startsWith(typed);
    if (!matches && !slipped) slips++;  // counted once until it's fixed
    slipped = !matches;
    LOGI("touch", "typing test: \"%s\" for \"%s\" (tap at %u, %u)", typed.c_str(), target.c_str(), x, y);
  } else if (result == keyboard::Result::Deleted) {
    slipped = !target.startsWith(typed);
  }
  if (typed == target) {  // the word is done: the next one
    word++;
    typed = "";
    keyboard::reset();
    if (word >= kWordCount) LOGI("touch", "typing test: %d slips", slips);
  }
  dirty = true;
}

}  // namespace

void open(bool welcome) {
  onboarding = welcome;
  open_ = true;
  showingResult = false;
  typing = false;
  roundNo = 1;
  all.clear();
  taps.clear();
  errors.clear();
  progress.clear();
  before = shell::touch.calibration();
  dirty = true;
}

bool active() {
  return open_;
}

bool loop(const shell::Input &input) {
  if (!open_) return false;
  if (input.boot) {  // leave: the welcome flow keeps what it has (and doesn't ask again)
    close(onboarding);
    return false;
  }
  if (input.key == 'T' && !typing) {  // developer aid: straight to the typing test
    typing = true;
    word = 0;
    typed = "";
    slips = 0;
    keyboard::reset();
    dirty = true;
  } else if (typing) {
    loopTyping(input);
  } else if (input.gesture == Touch::Gesture::Tap) {
    const uint16_t x = shell::touch.x(), y = shell::touch.y();
    if (showingResult) {
      if (y >= kButtonsTop) {
        if (x >= kW / 2) {  // Done: keep it, then try it on the keyboard
          shell::touch.setCalibration(shell::touch.calibration(), true);
          showingResult = false;
          typing = true;
          word = 0;
          typed = "";
          slips = 0;
          keyboard::reset();
          dirty = true;
          return false;
        }
        showingResult = false;
        taps.clear();
        roundNo++;
        dirty = true;
      }
    } else if (onboarding && x >= kW - kSkipW && y < kSkipH) {
      close(true);  // Skip: the defaults (or what the rounds so far fitted)
      return false;
    } else {
      int best = -1;
      float bestDistance = kMaxMiss;
      for (int i = 0; i < kTargets; i++) {
        const float d = hypotf(x - targetX(i), y - targetY(i));
        if (d < bestDistance) best = i, bestDistance = d;
      }
      if (best >= 0) {
        const Sample s = {targetX(best), targetY(best), shell::touch.rawX(), shell::touch.rawY(), x, y};
        taps.push_back(s);
        all.push_back(s);
        logTap(s);
        LOGI("touch", "calibration tap: cross (%d, %d), raw (%u, %u), landed (%u, %u)", s.targetX, s.targetY, s.rawX,
             s.rawY, x, y);
        if (static_cast<int>(taps.size()) >= kTargets) finishRound();
        dirty = true;
      }
    }
  }
  if (!dirty || epd.isBusy()) return false;
  dirty = false;
  return true;
}

void draw() {
  if (typing) drawTyping();
  else if (showingResult) drawResult();
  else drawRound();
}

}  // namespace calibrate
