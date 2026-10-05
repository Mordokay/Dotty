#include "ui.h"

#include <Fonts/FreeSans9pt7b.h>

#include "epd_display.h"

namespace ui {
namespace {

constexpr int16_t kW = EpdDisplay::kSize;
constexpr uint16_t kBlack = EpdDisplay::kBlack;
constexpr uint16_t kWhite = EpdDisplay::kWhite;

// Plain stand-ins for common non-ASCII punctuation (0 = drop the character).
char asciiFor(uint32_t cp) {
  switch (cp) {
    case 0x2018: case 0x2019: case 0x201B: case 0x2032: case 0x00B4: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
    case 0x00A0: case 0x3000: case 0x00B7: case 0x30FB: case 0x2022: return ' ';
    case 0xFF08: case 0x3010: case 0x300C: case 0x300E: case 0x300A: return '(';
    case 0xFF09: case 0x3011: case 0x300D: case 0x300F: case 0x300B: return ')';
    case 0xFF0C: case 0x3001: return ',';
    case 0xFF1A: return ':';
    case 0x2026: return '.';
    default: return 0;
  }
}

bool replaceAll(String &text, const char *from, const char *to) {
  if (text.indexOf(from) < 0) return false;
  text.replace(from, to);
  return true;
}

// The helpers below take text that is already printable().
int16_t rawWidth(Adafruit_GFX &gfx, const String &text) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx.setTextWrap(false);
  gfx.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);
  return w;
}

void rawCentered(Adafruit_GFX &gfx, const String &text, int16_t y) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx.setTextWrap(false);
  gfx.getTextBounds(text.c_str(), 0, y, &x1, &y1, &w, &h);
  gfx.setCursor((kW - w) / 2 - x1, y);
  gfx.print(text);
}

String rawFit(Adafruit_GFX &gfx, String text, int16_t maxWidth) {
  if (rawWidth(gfx, text) <= maxWidth) return text;
  while (text.length() > 1 && rawWidth(gfx, text + "...") > maxWidth) text.remove(text.length() - 1);
  text.trim();
  return text + "...";
}

}  // namespace

String printable(const String &text) {
  String out;
  out.reserve(text.length());
  for (size_t i = 0; i < text.length();) {
    const uint8_t c = text[i];
    if (c < 0x80) {
      out += c >= 32 ? static_cast<char>(c) : ' ';
      i++;
      continue;
    }
    // Decode one UTF-8 sequence.
    const int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    uint32_t cp = len == 1 ? 0 : c & (0x7F >> len);
    for (int k = 1; k < len && i + k < text.length(); k++) cp = (cp << 6) | (static_cast<uint8_t>(text[i + k]) & 0x3F);
    i += len;
    if (const char a = asciiFor(cp)) out += a;
  }
  // Brackets and quotes that held only dropped characters, then spacing.
  for (bool changed = true; changed;) {
    changed = false;
    for (const char *empty : {"()", "[]", "{}", "( )", "[ ]", "''", "\"\""}) changed |= replaceAll(out, empty, "");
    changed |= replaceAll(out, "  ", " ");
    changed |= replaceAll(out, "( ", "(");
    changed |= replaceAll(out, " )", ")");
    changed |= replaceAll(out, " ,", ",");
  }
  out.trim();
  while (out.startsWith("-") || out.startsWith(",")) {
    out.remove(0, 1);
    out.trim();
  }
  return out;
}

int16_t textWidth(Adafruit_GFX &gfx, const String &text) {
  return rawWidth(gfx, printable(text));
}

void drawCentered(Adafruit_GFX &gfx, const String &text, int16_t y) {
  rawCentered(gfx, printable(text), y);
}

String fitText(Adafruit_GFX &gfx, String text, int16_t maxWidth) {
  return rawFit(gfx, printable(text), maxWidth);
}

std::vector<String> wrapText(Adafruit_GFX &gfx, const String &text, int16_t maxWidth, int maxLines) {
  std::vector<String> lines;
  String rest = printable(text);
  while (rest.length() && static_cast<int>(lines.size()) < maxLines) {
    if (static_cast<int>(lines.size()) == maxLines - 1 || rawWidth(gfx, rest) <= maxWidth) {
      lines.push_back(rawFit(gfx, rest, maxWidth));  // last line: whatever is left, shortened
      break;
    }
    // The longest run of whole words that fits; a single overlong word is cut.
    int cut = -1;
    for (int space = rest.indexOf(' '); space > 0; space = rest.indexOf(' ', space + 1)) {
      if (rawWidth(gfx, rest.substring(0, space)) > maxWidth) break;
      cut = space;
    }
    if (cut < 0) {
      String piece = rest;
      while (piece.length() > 1 && rawWidth(gfx, piece) > maxWidth) piece.remove(piece.length() - 1);
      lines.push_back(piece);
      rest = rest.substring(piece.length());
    } else {
      lines.push_back(rest.substring(0, cut));
      rest = rest.substring(cut + 1);
    }
    rest.trim();
  }
  return lines;
}

int drawWrapped(Adafruit_GFX &gfx, const String &text, int16_t y, int16_t maxWidth, int maxLines,
                int16_t lineHeight) {
  const std::vector<String> lines = wrapText(gfx, text, maxWidth, maxLines);
  for (size_t i = 0; i < lines.size(); i++) rawCentered(gfx, lines[i], y + i * lineHeight);
  return lines.size();
}

void drawHeader(Adafruit_GFX &gfx, const char *label) {
  gfx.fillRect(0, 0, kW, 26, kBlack);
  gfx.setFont(&FreeSans9pt7b);
  gfx.setTextColor(kWhite);
  drawCentered(gfx, label, 18);
  gfx.setTextColor(kBlack);
}

void drawBattery(Adafruit_GFX &gfx, int16_t x, int16_t y, uint8_t percent) {
  const int16_t w = 26, h = 13;
  gfx.drawRect(x, y, w, h, kBlack);
  gfx.drawRect(x + 1, y + 1, w - 2, h - 2, kBlack);
  gfx.fillRect(x + w, y + 4, 3, h - 8, kBlack);  // terminal nub
  const int16_t fill = (w - 6) * min<uint8_t>(percent, 100) / 100;
  gfx.fillRect(x + 3, y + 3, fill, h - 6, kBlack);
}

void drawNote(Adafruit_GFX &gfx, int16_t x, int16_t y) {
  gfx.fillCircle(x + 3, y + 11, 3, kBlack);
  gfx.fillRect(x + 5, y, 2, 12, kBlack);
  gfx.fillTriangle(x + 7, y, x + 7, y + 6, x + 11, y + 5, kBlack);
}

String formatDuration(uint32_t ms) {
  char buf[12];
  const uint32_t s = ms / 1000;
  snprintf(buf, sizeof(buf), "%lu:%02lu", s / 60, s % 60);
  return buf;
}

}  // namespace ui
