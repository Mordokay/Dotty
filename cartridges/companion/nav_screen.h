#pragma once

#include <Adafruit_GFX.h>
#include <Arduino.h>

#include <string>
#include <vector>

#include "nav_route.h"

// Directions on the screen (spec: docs/superpowers/specs/2026-10-25-companion-directions-design.md):
// the navigation screen (✕ · destination · arrival, a big arrow, the metres to the turn, the street, what's
// left), the stop question, the lock-screen card, and the "Nearby" list.

struct NavView {
  std::string destination, street, clock;  // clock: the arrival time, "12:45"
  Arrow arrow = Arrow::Straight;
  int exit = 0;
  int toTurn = 0, left = 0, minutes = 0;
  bool confirm = false;  // "Stop the directions?" Yes / No
  bool waiting = false;  // no progress for 30 s: "Waiting for your iPhone..."
  std::string talk;      // a chat on the way: "Listening...", "Thinking...", "Percival is answering..." ("" = none)
  bool listening = false;
  bool saying = false;   // talk is the reply's words so far: shown two lines a page, the latest page
  int levelDb = -60;     // the microphone meter while listening
};
enum class NavHit { None, Stop, Yes, No };

void drawNav(const NavView &v);
NavHit navHit(uint16_t x, uint16_t y, const NavView &v);
void drawArrow(Adafruit_GFX &g, Arrow a, int exit, int16_t cx, int16_t cy, int16_t size, uint16_t color);
// The lock screen's widget (a 200x80 canvas): the arrow and the metres to the turn. Returns its height.
int16_t drawNavLockCard(Adafruit_GFX &canvas, const NavView &v);

struct NearbyRow {
  std::string name, detail;  // "Millennium ATM", "150 m - 2 min"
};
void drawNearby(const std::string &what, const std::vector<NearbyRow> &rows, int page, int pages);
// -2 back, -3 the page corner, a row on this page (0..3), -1 nothing.
int nearbyHit(uint16_t x, uint16_t y, int rows);
