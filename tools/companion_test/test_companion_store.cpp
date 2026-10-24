// uses: cartridges/companion/companion_store.cpp cartridges/companion/memory_store.cpp
// The companions: each one's profile (fields, limits, presets) and the list (ids, the 8 cap, the
// active one when one goes, moving piece 2a's data to Debug).
#include <algorithm>
#include <string>

#include "check.h"
#include "companion_store.h"

static size_t points(const std::string &s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) n++;
  return n;
}

int main() {
  // The presets (spec: "The ready-made four" + Debug).
  const Profile coach = presetProfile("coach"), friendP = presetProfile("friend"), partner = presetProfile("partner"),
                teacher = presetProfile("teacher"), debug = presetProfile("debug");
  CHECK("preset coach", coach.id == "coach" && coach.type == "coach" && coach.avatar == "dog" && coach.voice == "Puck");
  CHECK("preset friend", friendP.avatar == "cat" && friendP.voice == "Zephyr");
  CHECK("preset partner", partner.avatar == "girl" && partner.voice == "Achernar" &&
                              partner.personality.find("girlfriend") != std::string::npos);
  CHECK("preset teacher", teacher.avatar == "owl" && teacher.voice == "Charon");
  CHECK("preset debug: named and awake", debug.name == "Debug" && debug.awake() && debug.avatar == "robot");
  CHECK("presets: asleep until named", !coach.awake() && !friendP.awake() && !partner.awake() && !teacher.awake());
  CHECK("presets: traits and quirks", coach.traits.size() >= 3 && coach.quirks.size() >= 1 && !coach.speaking.empty() &&
                                          !coach.backstory.empty());

  // JSON round trip, with names from anywhere.
  Profile p = presetProfile("friend");
  p.name = "Zo\xC3\xAB \xF0\x9F\x8C\x99";  // "Zoë 🌙"
  Profile q;
  CHECK("round trip: parses", parseProfile(profileJson(p), q));
  CHECK("round trip: fields", q.name == p.name && q.personality == p.personality && q.traits == p.traits &&
                                  q.quirks == p.quirks && q.voice == p.voice && q.avatar == p.avatar && q.id == "friend");
  p.name = "\xEC\x95\x88\xEB\x85\x95";  // Korean
  CHECK("round trip: Korean name", parseProfile(profileJson(p), q) && q.name == p.name);
  CHECK("parse: garbage refused", !parseProfile("{nope", q));

  // setField: limits, lists, validation.
  std::string longText;
  while (points(longText) < 700) longText += "word ";
  CHECK("setField: personality", setField(p, "personality", longText) && points(p.personality) <= 600 &&
                                     p.personality.back() != ' ');
  CHECK("setField: name cut to 24", setField(p, "name", "Bartholomew Montgomery the Third") && points(p.name) <= 24);
  CHECK("setField: traits as lines, max 6 of 40",
        setField(p, "traits", "a\nb\nc\nd\ne\nf\ng\nthis trait is far longer than forty characters for sure") &&
            p.traits.size() == 6);
  CHECK("setField: quirks, empty lines dropped", setField(p, "quirks", "\nloves puns\n\n") && p.quirks.size() == 1 &&
                                                     p.quirks[0] == "loves puns");
  CHECK("setField: a known voice", setField(p, "voice", "Kore") && p.voice == "Kore");
  CHECK("setField: unknown voice refused", !setField(p, "voice", "Nobody") && p.voice == "Kore");
  CHECK("setField: avatar or photo", setField(p, "avatar", "photo") && setField(p, "avatar", "fox") && p.avatar == "fox");
  CHECK("setField: unknown avatar refused", !setField(p, "avatar", "unicorn"));
  CHECK("setField: unknown field refused", !setField(p, "shoeSize", "42"));

  // The list.
  CompanionList list;
  for (const char *t : {"coach", "friend", "partner", "teacher", "debug"}) CHECK("list: preset created", list.create(t) == t);
  CHECK_EQ("list: a preset only once", list.create("coach"), std::string(""));
  CHECK_EQ("list: first custom", list.create("custom"), std::string("c1"));
  CHECK_EQ("list: second custom", list.create("custom"), std::string("c2"));
  CHECK_EQ("list: third custom (8)", list.create("custom"), std::string("c3"));
  CHECK_EQ("list: full at 8", list.create("custom"), std::string(""));
  std::string active;
  CHECK("remove: a custom", list.remove("c3", active, "c3") && active == "c2");  // the last went: the one before
  CHECK_EQ("ids never reused", list.create("custom"), std::string("c4"));
  CHECK("remove: active in the middle → the next", list.remove("partner", active, "partner") && active == "teacher");
  CHECK("remove: not active → unchanged", list.remove("c1", active, "teacher") && active == "teacher");
  CompanionList copy;
  CHECK("list: save/load", copy.load(list.save()) && copy.order() == list.order());
  CHECK_EQ("list: next id survives the file", copy.create("custom"), std::string("c5"));
  CompanionList one;
  one.create("debug");
  CHECK("remove: never the last one", !one.remove("debug", active, "debug") && one.has("debug"));

  // Evolution: pinned lines never change by themselves.
  Profile e = presetProfile("partner");
  e.traits = {"affectionate", "supportive", "witty", "thoughtful", "teaser", "horny"};
  e.pins["traits"] = {"affectionate", "horny"};
  Profile er;
  CHECK("pins: round trip", parseProfile(profileJson(e), er) && er.pins["traits"] == e.pins["traits"]);
  Profile ev = e;
  CHECK("evolve: applied", applyEvolution(ev, "traits", "supportive\nwitty\nbrave\nthoughtful\nteaser"));
  // Pinned lines dropped by the evolution go back at their old place; over 6, the last unpinned one gives way.
  CHECK("evolve: pinned lines put back",
        ev.traits == (std::vector<std::string>{"affectionate", "supportive", "witty", "brave", "thoughtful", "horny"}));
  CHECK("evolve: the new one is there", std::find(ev.traits.begin(), ev.traits.end(), "brave") != ev.traits.end());
  {
    Profile k = presetProfile("partner");
    k.quirks = {"Calls you my king", "Hums when thinking"};
    k.pins["quirks"] = {"Calls you my king"};
    std::vector<std::string> kept;
    const bool ok = applyEvolution(k, "quirks", "Calls you Peter the Wise\nHums when thinking", &kept);
    CHECK("evolve: the pinned lines it couldn't change are reported",
          ok && kept == std::vector<std::string>{"Calls you my king"} &&
              k.quirks == (std::vector<std::string>{"Calls you my king", "Calls you Peter the Wise", "Hums when thinking"}));
    std::vector<std::string> none = {"stale"};
    applyEvolution(k, "quirks", "Calls you my king\nHums when thinking\nLaughs at puns", &none);
    CHECK("evolve: nothing pinned touched -> nothing reported", none.empty());
  }
  Profile prose = presetProfile("friend");
  prose.backstory = "Grew up by the sea.\nWorked in a bakery.";
  prose.pins["backstory"] = {"Grew up by the sea."};
  CHECK("evolve: prose keeps its pinned line",
        applyEvolution(prose, "backstory", "Worked in a bakery.\nNow runs every morning.") &&
            prose.backstory == "Grew up by the sea.\nWorked in a bakery.\nNow runs every morning.");
  Profile bul = presetProfile("friend");
  CHECK("evolve: list marks dropped", applyEvolution(bul, "speaking", "- Uses surf slang.\n* Talks fast.\n• Laughs a lot.") &&
                                          bul.speaking == "Uses surf slang.\nTalks fast.\nLaughs a lot.");
  CHECK("evolve: the review's (pinned) mark dropped",
        applyEvolution(bul, "speaking", "Uses surf slang. (pinned)\nTalks fast.") && bul.speaking == "Uses surf slang.\nTalks fast.");
  CHECK("evolve: unknown field refused", !applyEvolution(prose, "name", "Bob") && !applyEvolution(prose, "voice", "Kore"));
  CHECK_EQ("evolve: the log line",
           evolutionLine(1790000000, "traits", "a\nb", "a\nc", "she got braver"),
           std::string(R"({"t":1790000000,"field":"traits","before":"a\nb","after":"a\nc","why":"she got braver"})"));
  // The log, newest first; an undo hides itself and marks the change it undid.
  const std::string log = evolutionLine(100, "traits", "a", "b", "x") + "\n" + "not json\n" +
                          evolutionLine(200, "backstory", "c", "d", "y") + "\n" +
                          evolutionLine(300, "traits", "b", "a", "undone", 100) + "\n";
  const std::vector<Evolution> evs = parseEvolutions(log);
  CHECK("log: two changes, newest first", evs.size() == 2 && evs[0].t == 200 && evs[1].t == 100);
  CHECK("log: the undone one is marked", evs.size() == 2 && !evs[0].undone && evs[1].undone);
  CHECK("log: fields read back", evs.size() == 2 && evs[1].field == "traits" && evs[1].before == "a" &&
                                     evs[1].after == "b" && evs[1].why == "x");
  // Pins travel as line numbers (a pinned sentence can be longer than a BLE command), kept as text.
  Profile pn = presetProfile("friend");
  pn.backstory = "Grew up by the sea.\nWorked in a bakery.\nRuns.";
  CHECK("pins: by line number", setPins(pn, "backstory", {0, 2, 9}) &&
                                    pn.pins["backstory"] == (std::vector<std::string>{"Grew up by the sea.", "Runs."}));
  CHECK("pins: read back as numbers", pinnedLines(pn, "backstory") == (std::vector<int>{0, 2}));
  pn.backstory = "Runs.\nGrew up by the sea.";
  CHECK("pins: follow their line when it moves", pinnedLines(pn, "backstory") == (std::vector<int>{0, 1}));
  CHECK("pins: unknown field refused", !setPins(pn, "voice", {0}));
  CHECK("pins: none clears", setPins(pn, "backstory", {}) && pinnedLines(pn, "backstory").empty());
  CHECK_EQ("lines: a list", fieldText(e, "traits"), std::string("affectionate\nsupportive\nwitty\nthoughtful\nteaser\nhorny"));

  // Stepping through the companions (Dotty's settings row): never a division by zero.
  CHECK_EQ("step: next", stepCompanion({"a", "b", "c"}, "b", 1), std::string("c"));
  CHECK_EQ("step: wraps", stepCompanion({"a", "b", "c"}, "c", 1), std::string("a"));
  CHECK_EQ("step: back", stepCompanion({"a", "b", "c"}, "a", -1), std::string("c"));
  CHECK_EQ("step: one companion stays", stepCompanion({"a"}, "a", 1), std::string("a"));
  CHECK_EQ("step: none (no card)", stepCompanion({}, "debug", 1), std::string("debug"));

  // Moving piece 2a's data into Debug's folder (idempotent, resumable).
  const std::vector<Move> fresh = migrationPlan(true, true, false, false);
  CHECK("migration: two moves", fresh.size() == 2 && fresh[0].from == "memory" && fresh[0].to == "companions/debug/memory" &&
                                    fresh[1].from == "history" && fresh[1].to == "companions/debug/history");
  CHECK("migration: done already", migrationPlan(false, false, true, true).empty());
  const std::vector<Move> half = migrationPlan(false, true, true, false);
  CHECK("migration: half done → the rest", half.size() == 1 && half[0].from == "history");
  CHECK("migration: never over existing data", migrationPlan(true, false, true, false).empty());
  return failures ? 1 : 0;
}
