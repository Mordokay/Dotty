#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "companion_store.h"
#include "nav_route.h"
#include "script_writer.h"

// Directions, in words (spec: docs/superpowers/specs/2026-10-25-companion-directions-design.md): the text
// model writes every step's line in the companion's voice (recorded into clips ahead of you), the notes
// that make it speak live (the start, a re-route, arrival, where you are when you ask something), what the
// instruction says about places and a trip, and the answers about places nearby. Plain C++ (host-tested).

// One generateContent body asking for a JSON array: one line per spoken step, the instruction first and
// clear, the flavour after, at most ~20 words, the distance said = the mode's trigger distance.
std::string navLinesRequest(const Profile &who, const std::string &memorySection, const NavRoute &r);
// The answer as one line per step (index = step; "" for an unspoken step, a missing or failed line).
std::vector<std::string> parseNavLines(const Written &w, const NavRoute &r);

std::string distanceWords(int metres);  // "350 m", "1.4 km"
std::string navStartNote(const NavRoute &r, int64_t now);
std::string navRerouteNote(const NavRoute &r);
std::string navArrivedNote(const std::string &destination);
std::string navProgressNote(const NavRoute &r, const NavProgress &p, int64_t now);  // goes before a question
// For the instruction: the saved places, transit, and a trip under way (trip/p null when none).
std::string navSection(const std::vector<std::string> &placeNames, const NavRoute *trip, const NavProgress *p,
                       int64_t now);

struct NearbyPlace {
  int n = 0;
  std::string name, category, street, direction, phone;
  int distance = 0, minutes = 0;
};
bool parseNearbyPlace(JsonObjectConst msg, NearbyPlace &out);
std::string nearbyResult(const std::string &what, const std::vector<NearbyPlace> &places);  // a spoken tool result
std::string nearbyRowDetail(const NearbyPlace &p);  // "150 m - 2 min"
// "nearby 2" (navigate_to after find_nearby) → that place's name; `missing` when there's no such result.
// Anything else comes back as it is.
std::string nearbyQuery(const std::string &place, const std::vector<NearbyPlace> &last, bool &missing);
