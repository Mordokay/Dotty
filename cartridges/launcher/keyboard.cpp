#include "keyboard.h"

#include <Fonts/FreeSans9pt7b.h>

#include <vector>

#include "shell.h"

namespace keyboard {
namespace {

using shell::epd;

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;
constexpr uint32_t kCapsLockMs = 800;  // shift tapped again within this: caps lock

enum class Kind : uint8_t { Char, Shift, Back, Space, Layer };
enum Layer : uint8_t { kLetters, kNumbers, kSymbols };
struct Key {
  int8_t row, col, span;
  Kind kind;
  char ch;         // Char
  Layer to;        // Layer: the page it opens
  const char *label;
};

std::vector<Key> layout(Layer layer) {
  std::vector<Key> keys;
  auto chars = [&](int row, int col, const char *s) {
    for (int i = 0; s[i]; i++) keys.push_back({int8_t(row), int8_t(col + i), 1, Kind::Char, s[i], kLetters, nullptr});
  };
  auto special = [&](int row, int col, int span, Kind kind, Layer to = kLetters, const char *label = nullptr) {
    keys.push_back({int8_t(row), int8_t(col), int8_t(span), kind, 0, to, label});
  };
  if (layer == kLetters) {
    chars(0, 0, "qwertyui");
    chars(1, 0, "asdfghop");
    chars(2, 0, "zxcvbjkl");
    special(3, 0, 1, Kind::Shift);
    chars(3, 1, "nm");
    special(3, 3, 1, Kind::Layer, kNumbers, "123");
  } else {
    // Digits and the common punctuation; "#+=" has brackets, maths and the rare ones.
    if (layer == kNumbers) {
      chars(0, 0, "12345678");
      chars(1, 0, "90-/:;()");
    } else {
      chars(0, 0, "[]{}#%^*");
      chars(1, 0, "+=\\|<>~`");
    }
    chars(2, 0, "$&@\".,?!");
    special(3, 0, 1, Kind::Layer, layer == kNumbers ? kSymbols : kNumbers, layer == kNumbers ? "#+=" : "123");
    chars(3, 1, "'_");
    special(3, 3, 1, Kind::Layer, kLetters, "ABC");
  }
  special(3, 4, 3, Kind::Space);
  special(3, 7, 1, Kind::Back);
  return keys;
}

// Column edges. The touch chip reads the outer strips of the screen as one position each (see
// touch.h), so the first and last columns end/start where those strips do (from the
// calibration; on the prototype the right strip starts at x 166, and a tap on the right of a
// 25 px "u" read as "i"); the columns between share the rest evenly.
int16_t edges[kCols + 1];

void computeEdges() {
  float left, right;
  Touch::bands(shell::touch.calibration(), left, right);
  edges[0] = 0;
  edges[kCols] = kW;
  edges[1] = constrain(lroundf(left), kKeyW - 4, kKeyW + 10);
  edges[kCols - 1] = constrain(lroundf(right), kW - kKeyW - 10, kW - kKeyW + 4);
  for (int i = 2; i < kCols - 1; i++) edges[i] = edges[1] + (edges[kCols - 1] - edges[1]) * (i - 1) / (kCols - 2);
}

int16_t keyX(const Key &k) { return edges[k.col]; }
int16_t keyW(const Key &k) { return edges[k.col + k.span] - edges[k.col]; }

Layer layer = kLetters;
bool shift = false, capsLock = false;
uint32_t shiftAt = 0;

// A key's frame: neighbours share their 1 px border (the right edge stays on the screen).
void keyFrame(const Key &k, bool dark) {
  const int16_t x = keyX(k), y = kTop + k.row * kKeyH, w = min<int16_t>(keyW(k) + 1, kW - x);
  if (dark) epd.fillRect(x, y, w, kKeyH + 1, kBlack);
  else epd.drawRect(x, y, w, kKeyH + 1, kBlack);
}

// Centred across, on one baseline per row (so "g" and "y" don't jump about).
void keyLabel(const Key &k, const String &label, bool dark) {
  int16_t bx, by;
  uint16_t bw, bh;
  epd.getTextBounds(label, 0, 0, &bx, &by, &bw, &bh);
  const int16_t cx = keyX(k) + keyW(k) / 2, baseline = kTop + k.row * kKeyH + 21;
  epd.setTextColor(dark ? kWhite : kBlack);
  epd.setCursor(cx - bw / 2 - bx, baseline);
  epd.print(label);
  epd.setTextColor(kBlack);
}

void drawKeys() {
  for (const Key &k : layout(layer)) {
    const int16_t cx = keyX(k) + keyW(k) / 2, cy = kTop + k.row * kKeyH + kKeyH / 2;
    switch (k.kind) {
      case Kind::Char:
        keyFrame(k, false);
        epd.setFont(&FreeSans9pt7b);
        keyLabel(k, String(shift && isalpha(k.ch) ? static_cast<char>(toupper(k.ch)) : k.ch), false);
        break;
      case Kind::Shift: {  // an up arrow, dark while on; caps lock adds a bar under it
        keyFrame(k, shift);
        const uint16_t ink = shift ? kWhite : kBlack;
        epd.fillTriangle(cx, cy - 8, cx - 8, cy, cx + 8, cy, ink);
        epd.fillRect(cx - 3, cy, 7, 5, ink);
        if (capsLock) epd.fillRect(cx - 5, cy + 7, 11, 2, ink);
        break;
      }
      case Kind::Back:  // an arrow-shaped tag pointing left with a cross in it
        keyFrame(k, false);
        epd.drawLine(cx - 9, cy, cx - 4, cy - 5, kBlack);
        epd.drawLine(cx - 9, cy, cx - 4, cy + 5, kBlack);
        epd.drawFastHLine(cx - 4, cy - 5, 13, kBlack);
        epd.drawFastHLine(cx - 4, cy + 5, 13, kBlack);
        epd.drawFastVLine(cx + 9, cy - 5, 11, kBlack);
        epd.drawLine(cx - 1, cy - 2, cx + 4, cy + 3, kBlack);
        epd.drawLine(cx - 1, cy + 2, cx + 4, cy - 3, kBlack);
        break;
      case Kind::Space:  // a bracket lying down
        keyFrame(k, false);
        epd.drawFastHLine(cx - 16, cy + 4, 33, kBlack);
        epd.drawFastVLine(cx - 16, cy + 1, 3, kBlack);
        epd.drawFastVLine(cx + 16, cy + 1, 3, kBlack);
        break;
      case Kind::Layer:  // "123" / "ABC" / "#+=": the small built-in font fits 25 px
        keyFrame(k, true);
        epd.setFont(nullptr);
        epd.setTextColor(kWhite);
        epd.setCursor(cx - 8, cy - 3);
        epd.print(k.label);
        epd.setTextColor(kBlack);
        break;
    }
  }
}

Result type(char c, String &text, size_t max) {
  if (text.length() >= max) return Result::None;
  text += c;
  if (shift && !capsLock && isalpha(c)) shift = false;
  return Result::Typed;
}

}  // namespace

void reset() {
  layer = kLetters;
  shift = capsLock = false;
}

void draw() {
  computeEdges();
  for (const Key &k : layout(layer)) {
    const int16_t cx = keyX(k) + keyW(k) / 2, cy = kTop + k.row * kKeyH + kKeyH / 2;
    switch (k.kind) {
      case Kind::Char:
        keyFrame(k, false);
        epd.setFont(&FreeSans9pt7b);
        keyLabel(k, String(shift && isalpha(k.ch) ? static_cast<char>(toupper(k.ch)) : k.ch), false);
        break;
      case Kind::Shift: {  // an up arrow, dark while on; caps lock adds a bar under it
        keyFrame(k, shift);
        const uint16_t ink = shift ? kWhite : kBlack;
        epd.fillTriangle(cx, cy - 8, cx - 8, cy, cx + 8, cy, ink);
        epd.fillRect(cx - 3, cy, 7, 5, ink);
        if (capsLock) epd.fillRect(cx - 5, cy + 7, 11, 2, ink);
        break;
      }
      case Kind::Back:  // an arrow-shaped tag pointing left with a cross in it
        keyFrame(k, false);
        epd.drawLine(cx - 9, cy, cx - 4, cy - 5, kBlack);
        epd.drawLine(cx - 9, cy, cx - 4, cy + 5, kBlack);
        epd.drawFastHLine(cx - 4, cy - 5, 13, kBlack);
        epd.drawFastHLine(cx - 4, cy + 5, 13, kBlack);
        epd.drawFastVLine(cx + 9, cy - 5, 11, kBlack);
        epd.drawLine(cx - 1, cy - 2, cx + 4, cy + 3, kBlack);
        epd.drawLine(cx - 1, cy + 2, cx + 4, cy - 3, kBlack);
        break;
      case Kind::Space:  // a bracket lying down
        keyFrame(k, false);
        epd.drawFastHLine(cx - 16, cy + 4, 33, kBlack);
        epd.drawFastVLine(cx - 16, cy + 1, 3, kBlack);
        epd.drawFastVLine(cx + 16, cy + 1, 3, kBlack);
        break;
      case Kind::Layer:  // "123" / "ABC" / "#+=": the small built-in font fits 25 px
        keyFrame(k, true);
        epd.setFont(nullptr);
        epd.setTextColor(kWhite);
        epd.setCursor(cx - 8, cy - 3);
        epd.print(k.label);
        epd.setTextColor(kBlack);
        break;
    }
  }
}

Result tap(uint16_t x, uint16_t y, String &text, size_t max) {
  if (y < kTop) return Result::None;
  computeEdges();
  int col = 0;
  while (col < kCols - 1 && x >= edges[col + 1]) col++;
  const int row = min(kRows - 1, (y - kTop) / kKeyH);
  for (const Key &k : layout(layer)) {
    if (k.row != row || col < k.col || col >= k.col + k.span) continue;
    switch (k.kind) {
      case Kind::Char:
        return type(shift && isalpha(k.ch) ? toupper(k.ch) : k.ch, text, max);
      case Kind::Shift:
        if (!shift) {
          shift = true;
          shiftAt = millis();
        } else if (!capsLock && millis() - shiftAt < kCapsLockMs) {
          capsLock = true;
        } else {
          shift = capsLock = false;
        }
        return Result::Changed;
      case Kind::Back:
        if (text.isEmpty()) return Result::None;
        text.remove(text.length() - 1);
        return Result::Deleted;
      case Kind::Space:
        return type(' ', text, max);
      case Kind::Layer:
        layer = k.to;
        shift = capsLock = false;
        return Result::Changed;
    }
  }
  return Result::None;  // between keys
}

Result key(char c, String &text, size_t max) {
  if (c == '^') {
    shift = !shift;
    return Result::Changed;
  }
  if (c == '#') {
    layer = static_cast<Layer>((layer + 1) % 3);
    return Result::Changed;
  }
  if (isalpha(c)) return type(c, text, max);
  return Result::None;
}

}  // namespace keyboard
