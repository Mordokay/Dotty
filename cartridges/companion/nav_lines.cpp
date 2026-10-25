#include "nav_lines.h"

#include <ArduinoJson.h>

namespace {

std::string minutesWords(int64_t seconds) {
  const int64_t m = seconds <= 0 ? 0 : (seconds + 30) / 60;
  return std::to_string(m) + (m == 1 ? " minute" : " minutes");
}

int firstSpoken(const NavRoute &r) {
  for (int i = 0; i < r.count(); i++)
    if (r.at(i).spoken()) return i;
  return -1;
}

std::string lower(std::string s) {
  for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

std::string distanceWords(int metres) {
  if (metres < 1000) return std::to_string(metres) + " m";
  char out[16];
  snprintf(out, sizeof(out), "%.1f km", metres / 1000.0);
  return out;
}

std::string navTripTopics(int route) {
  static const char *const kTopics[] = {"street life and the people around",
                                        "smells and food",
                                        "history and old stories",
                                        "music and sounds",
                                        "buildings and architecture",
                                        "the sky and the light",
                                        "sport and keeping fit",
                                        "plans and daydreams",
                                        "animals (not birds)",
                                        "your own little worries and quirks",
                                        "trivia and odd facts",
                                        "games and challenges on the way",
                                        "the weather and the seasons",
                                        "childhood memories",
                                        "romance and gossip",
                                        "science and how things work",
                                        "art and colours",
                                        "travel and faraway places",
                                        "your own secret ambitions",
                                        "superstitions and legends",
                                        "fashion and what people wear",
                                        "tiny mysteries along the way"};
  const int n = sizeof(kTopics) / sizeof(kTopics[0]);
  const unsigned u = static_cast<unsigned>(route) * 2654435761u;  // spread consecutive route numbers
  const int a = static_cast<int>(u % n), b = (a + 1 + static_cast<int>((u / n) % (n - 1))) % n;
  return std::string(kTopics[a]) + "; " + kTopics[b];
}

std::string navLinesRequest(const Profile &who, const std::string &memorySection, const NavRoute &r) {
  const std::string name = who.awake() ? who.name : "the companion";
  const int trigger = triggerMetres(r.mode);
  int spoken = 0;
  std::string steps;
  for (int i = 0; i < r.count(); i++) {
    if (!r.at(i).spoken()) continue;
    spoken++;
    steps += std::to_string(spoken) + ". " + r.text(i);
    if (!r.street(i).empty()) steps += " (street: " + r.street(i) + ")";
    if (!r.poi(i).empty()) steps += "; passing: " + r.poi(i);
    const int stretch = stretchBefore(r, i);
    if (stretch > 0) steps += "; walked before it: " + distanceWords(stretch);
    if (stretch >= longStretch(r.mode)) steps += " (long: a halfway remark is possible)";
    steps += "\n";
  }
  std::string system = writerCharacter(who, memorySection);
  system += " " + name + " is guiding the user (" + std::string(modeName(r.mode)) + ") to " + r.destination +
            ". Write a JSON array of " + std::to_string(spoken) +
            " objects, one per step in order: {\"turn\": the line said about " + std::to_string(trigger) +
            " metres before that step, \"mid\": a halfway remark or \"\"}. Each turn line: the instruction, plain and "
            "clear, and around it something spontaneous and fun in " + name + "'s own voice. Vary where the "
            "instruction sits: the start, the middle or the end of the line. At the start it may say the distance "
            "(\"In about " + std::to_string(trigger) + " metres, turn left onto Rua Augusta\"); in the middle or at the "
            "end, no distance (\"turn left at the next corner\", \"the left turn coming up\"). Every line different "
            "in kind: an odd "
            "little thing you just noticed (made up, playful), a warning, a joke, encouragement, a thought about the "
            "street or the place. This trip, lean on " + navTripTopics(r.route()) + ". Invent your own: never reuse a "
            "sentence, a joke, an image or a sign-off from one line in another. " +
            (spoken > 1 ? "Only lines 1 and " + std::to_string(spoken) + " may" : std::string("Only line 1 may")) +
            " call the user by a title or pet name (whatever your character usually calls them); every other line "
            "speaks without any form of address. When a step says "
            "what you pass there, mention it now and then (not every time), as a friend would; only those places, "
            "never invent a place or a name. When the weather is given, let it colour a line or two, never every "
            "line. At most 40 words a line. \"mid\" only for steps marked long, on about half of them: something said "
            "halfway along that stretch, like a friend walking alongside (how they're doing, a drink, a rest), at "
            "most 15 words, no directions in it; else \"\". The last step is "
            "the arrival. Spoken English, first person, no emoji or stage directions. Only the JSON array.";
  JsonDocument doc;
  doc["systemInstruction"]["parts"][0]["text"] = system;
  doc["contents"][0]["role"] = "user";
  doc["contents"][0]["parts"][0]["text"] = "Going to " + r.destination + " (" + distanceWords(r.distance) + ")." +
                                           (r.weather.empty() ? std::string() : " The weather: " + r.weather + ".") +
                                           " Each turn line is said " + std::to_string(trigger) +
                                           " metres before its step. The steps:\n" + steps;
  doc["generationConfig"]["maxOutputTokens"] = 200 + 150 * spoken;
  doc["generationConfig"]["temperature"] = 1.1;  // spontaneous, not samey
  doc["generationConfig"]["responseMimeType"] = "application/json";
  std::string out;
  serializeJson(doc, out);
  return out;
}

NavLineSet parseNavLines(const Written &w, const NavRoute &r) {
  NavLineSet out;
  out.turn.assign(r.count(), "");
  out.mid.assign(r.count(), "");
  if (!w.ok) return out;
  std::string text = w.text;
  const size_t open = text.find('['), close = text.rfind(']');  // code fences and chatter around the array
  if (open == std::string::npos || close == std::string::npos || close < open) return out;
  text = text.substr(open, close - open + 1);
  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) return out;
  JsonArrayConst lines = doc.as<JsonArrayConst>();
  size_t next = 0;
  for (int i = 0; i < r.count() && next < lines.size(); i++) {
    if (!r.at(i).spoken()) continue;
    JsonVariantConst item = lines[next++];
    if (item.is<const char *>()) {  // a plain string: just the turn
      out.turn[i] = item.as<const char *>();
      continue;
    }
    out.turn[i] = item["turn"] | "";
    if (stretchBefore(r, i) >= longStretch(r.mode)) out.mid[i] = item["mid"] | "";
  }
  return out;
}

std::string navStartNote(const NavRoute &r, int64_t now) {
  const int first = firstSpoken(r);
  std::string s = "(Dotty, not the user: the directions are ready.) " + std::string(modeName(r.mode)) + " to " +
                  r.destination + ", " + distanceWords(r.distance) + ", about " + minutesWords(r.eta - now) + ".";
  if (first >= 0) s += " The first step: " + r.text(first) + ".";
  s += " Tell the user in a few words of your own, then the first step, clearly.";
  return s;
}

std::string navRerouteNote(const NavRoute &r) {
  const int first = firstSpoken(r);
  std::string s = "(Dotty, not the user: they went off the route; here's a new one to " + r.destination + ", " +
                  distanceWords(r.distance) + ".)";
  if (first >= 0) s += " The first step now: " + r.text(first) + ".";
  s += " Tell them in a few words, then that step, clearly.";
  return s;
}

std::string navArrivedNote(const std::string &destination, const std::string &next) {
  if (!next.empty())  // a stop on the way: the next leg is coming
    return "(Dotty, not the user: they have arrived at " + destination + ", their stop on the way; next they go on to " +
           next + ", and that route is on its way.) Say so in a few words of your own.";
  return "(Dotty, not the user: they have arrived at " + destination +
         "; the directions are over.) Say so in a few words of your own.";
}

namespace {

std::string tripWords(const NavRoute &r, const NavProgress &p, int64_t now) {
  std::string s = std::string(modeName(r.mode)) + " to " + r.destination + ", " + distanceWords(p.left) + " and " +
                  minutesWords(p.eta - now) + " left";
  if (p.step >= 0 && p.step < r.count() && r.at(p.step).spoken())
    s += "; next: " + r.text(p.step) + " in " + distanceWords(p.toTurn);
  return s;
}

}  // namespace

std::string navProgressNote(const NavRoute &r, const NavProgress &p, int64_t now) {
  return "(Dotty, not the user: directions now: " + tripWords(r, p, now) + ".)";
}

std::string navSection(const std::vector<std::string> &placeNames, const NavRoute *trip, const NavProgress *p,
                       int64_t now) {
  std::string s;
  if (!placeNames.empty()) {
    s = "Saved places: ";
    for (size_t i = 0; i < placeNames.size(); i++) s += (i ? ", " : "") + placeNames[i];
    s += ".\n";
  }
  s += "Directions need the user's iPhone nearby (it finds the way). Walking unless they say ride, cycle or drive. "
       "You can't plan public transport yet: say so and offer to walk them or to open Apple Maps on the iPhone.";
  if (trip && p) s += "\nDirections under way: " + tripWords(*trip, *p, now) + ".";
  return s;
}

bool parseNearbyPlace(JsonObjectConst msg, NearbyPlace &out) {
  out.n = msg["n"] | 0;
  out.name = msg["name"] | "";
  out.category = msg["category"] | "";
  out.street = msg["street"] | "";
  out.direction = msg["direction"] | "";
  out.phone = msg["phone"] | "";
  out.distance = msg["distance"] | 0;
  out.minutes = msg["minutes"] | 0;
  return out.n > 0 && !out.name.empty();
}

std::string nearbyRowDetail(const NearbyPlace &p) {
  return distanceWords(p.distance) + " - " + std::to_string(p.minutes) + " min";
}

std::string nearbyResult(const std::string &what, const std::vector<NearbyPlace> &places) {
  if (places.empty())
    return "Nothing found nearby for \"" + what + "\", even a little further out. Tell the user briefly.";
  std::string s = "Places near the user for \"" + what + "\", nearest first:";
  bool hospital = lower(what).find("hospital") != std::string::npos;
  for (const NearbyPlace &p : places) {
    s += "\n" + std::to_string(p.n) + ". " + p.name;
    if (!p.street.empty()) s += ", " + p.street;
    s += ": " + distanceWords(p.distance) + ", " + std::to_string(p.minutes) + " min, " + p.direction;
    if (lower(p.category).find("hospital") != std::string::npos) hospital = true;
  }
  s += "\nTell the user about the nearest one or two in your own words and offer to take them there (\"the second "
       "one\" is fine). You don't know their hours: never say whether a place is closed or not.";
  if (hospital) s += " If it sounds urgent, tell them plainly to call 112 first.";
  return s;
}

std::string nearbyQuery(const std::string &place, const std::vector<NearbyPlace> &last, bool &missing) {
  missing = false;
  const std::string l = lower(place);
  if (l.rfind("nearby ", 0) != 0) return place;
  const int n = atoi(l.c_str() + 7);
  for (const NearbyPlace &p : last)
    if (p.n == n) return p.name;
  missing = true;
  return place;
}
