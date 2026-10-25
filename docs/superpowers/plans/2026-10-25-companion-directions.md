# Companion Directions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** "Walk me home" / "is there an ATM nearby?": the iPhone finds and routes, follows you with the phone
locked, and Dotty shows the next turn while the companion speaks every instruction in character.

**Architecture:** The iPhone does places, search, routing, location, step tracking, off-route and arrival
(`NavigationService`, app-wide like `RelayService`). Dotty holds the trip: three Live tools, the navigation
screen, clips written by the text model and recorded by Live through the existing line pipeline
(`LineJob`), played at a trigger distance; live lines for the start, re-routes and arrival. Plain logic on
both sides is host-tested (C++ `tools/companion_test`, Swift `tools/relay_swift_check`).

**Tech Stack:** ESP32-S3 Arduino (pioarduino), ArduinoJson 7, Adafruit GFX; SwiftUI, MapKit, CoreLocation,
CoreBluetooth; Gemini Live + `gemini-flash-lite-latest`.

**Spec:** `docs/superpowers/specs/2026-10-25-companion-directions-design.md`

## Global Constraints

- Ships as **Companion 0.8.0** (`DOTTY_CARTRIDGE`) + an app update; ends with commit → push → catalog release.
- Logging only with `LOGI/LOGW/LOGE`; never log API keys (last 4 at most).
- BLE commands ≤ 512 bytes: a route's steps go one per `nav.step`, nearby results one per `nav.place`.
- Internal RAM is the session's limit: a route's texts live in **one `std::string` buffer reserved ≥ 4,096
  bytes** (the companion build's PSRAM threshold is `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`) with offsets,
  never a string per step.
- Numbers (spec): trigger distance **40 m walking / 100 m cycling / 300 m driving**; progress sent on a
  change of **10 / 25 / 50 m** or a step change; off route **30 / 50 / 80 m for 8 s**; re-route **≤ every 20 s**,
  spoken **≤ once a minute**; arrival **25 m (40 driving)**; app waits **2 min** for Dotty; Dotty shows
  "Waiting for your iPhone…" after **30 s** without progress and ends at **2 min**; clips **≥ 3 steps ahead**;
  lines **≤ ~20 words, the instruction first**; screen redraw on **≥ 10 m** or a step change, **≤ every 3 s**;
  stop question gone after **10 s**; nearby radius **1 km on foot / 5 km driving**, once more at double,
  **≤ 5 results**; the nearby list closes after **2 min** untouched.
- Location stays on the iPhone; only instructions, street names and place names go to Gemini.
- No opening hours are ever claimed; nothing dials an emergency number (an urgent request → "call 112").
- Swift 6: CoreLocation/MapKit delegate callbacks run off the main actor — delegates are `nonisolated` and hop
  to `@MainActor` (the `SpeechAuth` launch crash came from MainActor closures called off main).
- Signing: the personal team only. Build check: `xcodebuild -project ios/Dotty.xcodeproj -scheme Dotty
  -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build`.

## Messages (fixed here; every task uses these names)

| From → to | Message | Fields |
| --- | --- | --- |
| Dotty → app (event) | `nav.request` | `req` (Dotty's counter), `query`, `mode` (`walking`/`cycling`/`driving`) |
| Dotty → app (event) | `nav.nearby` | `req`, `what`, `mode` |
| Dotty → app (event) | `nav.stop` | — |
| app → Dotty | `nav.route` | `req` (0 for a re-route), `route` (the app's counter, new on every route), `ok`, `error?`, `choices?` [{`n`,`name`,`distance`}], `destination`, `mode`, `distance` (m), `eta` (local epoch), `count`, `reroute?` |
| app → Dotty | `nav.step` | `route`, `i`, `text`, `street`, `distance`, `arrow`, `exit?` |
| app → Dotty | `nav.progress` | `route`, `step`, `toTurn` (m), `left` (m), `eta` |
| app → Dotty | `nav.arrived` / `nav.ended` | `route` / `route`, `why` |
| app → Dotty | `nav.places` + `nav.place` | `req`, `ok`, `error?`, `count` + `req`, `n`, `name`, `category`, `street`, `distance`, `minutes`, `direction`, `phone?` |
| app → Dotty | `companion.places` | `names` (≤ 20) |

Arrow names: `straight`, `slight-left`, `left`, `sharp-left`, `slight-right`, `right`, `sharp-right`,
`uturn`, `roundabout` (with `exit`), `arrive`.

## Review Focus

1. A `nav.progress` / `nav.step` for an older `route` (a re-route raced it) → ignored; clips of the old route never play. (Task 2)
2. A route whose steps don't all arrive (`count` not reached within 3 s) → the companion says the route didn't come through; no trip starts. (Task 2, Task 6)
3. GPS noise around the trigger (`toTurn` 45 → 38 → 42 → 35) → the clip plays once. (Task 2)
4. MapKit's empty first step ("" with distance 0) → no clip, no arrow, not counted as a turn. (Task 2, Task 8)
5. "Take me to the second one" with no nearby results kept → a spoken error, not a route to nowhere. (Task 3, Task 9)

---

### Task 1: The background location probe

**Files:**
- Create: `ios/Dotty/Features/Navigation/LocationProbe.swift`
- Modify: `ios/Dotty/Bluetooth/DottyLink.swift` (route the `nav.probe` event), `ios/Dotty-Info.plist`
  (`UIBackgroundModes` += `location`; `NSLocationAlwaysAndWhenInUseUsageDescription` = "Dotty guides you with
  directions while your iPhone is in your pocket."), `ios/Dotty/Features/Relay/RelayLogView.swift` (a "Location
  probe" row), `cartridges/companion/main.cpp` (dev key `\`` sends `{event: nav.probe}`)

**Produces:** a yes/no answer (written to the ledger and later to CLAUDE.md) to "can a locked iPhone, woken by
a Dotty event, start location updates and keep them for 2 minutes", and whether `MKDirections` returns cycling
routes on iOS 26.

- [ ] **Step 1:** `LocationProbe`: on `nav.probe`, request `.authorizedAlways` if not granted (foreground only),
  start a `CLLocationManager` (best accuracy, `allowsBackgroundLocationUpdates = true`,
  `showsBackgroundLocationIndicator = true`), log authorization, app state, fix count and accuracy every 15 s for
  120 s to the Relay log; then one walking and one cycling `MKDirections` request to a point 1 km away, logging
  step counts or the error.
- [ ] **Step 2:** Build (`xcodebuild … build`). Expected: `** BUILD SUCCEEDED **`.
- [ ] **Step 3:** With the user: grant Always, lock the iPhone, send `` ` `` to Dotty; read the Relay log.
  Expected: fixes keep arriving with the app `background`. If not: stop and report (the trip must then start
  from the app).
- [ ] **Step 4:** Commit `Directions probe: background location from a Dotty event`.

### Task 2: The trip on Dotty (`nav_route.*`, host-tested)

**Files:** Create `cartridges/companion/nav_route.h`, `nav_route.cpp`; Test
`tools/companion_test/test_nav_route.cpp` (`// uses: cartridges/companion/nav_route.cpp`).

**Interfaces (Produces):**
```cpp
enum class NavMode { Walking, Cycling, Driving };
enum class Arrow { Straight, SlightLeft, Left, SharpLeft, SlightRight, Right, SharpRight, UTurn, Roundabout, Arrive };
bool parseMode(const std::string &, NavMode &);  const char *modeName(NavMode);
bool parseArrow(const std::string &, Arrow &);
int triggerMetres(NavMode);                       // 40 / 100 / 300
struct NavStep { std::string text() const; std::string street() const; int distance; Arrow arrow; int exit; bool spoken() const; };
class NavRoute {                                  // one route (texts in one buffer, reserve(4096))
 public:
  bool begin(JsonObjectConst navRoute);           // a new route number; false when !ok
  bool step(JsonObjectConst navStep);             // false: another route, out of range
  bool complete() const;  int route() const;  int count() const;  const NavStep &at(int i) const;
  std::string destination, error; NavMode mode; int distance; int64_t eta; bool reroute;
  std::vector<std::pair<std::string, int>> choices;
};
struct NavProgress { int route = 0, step = 0, toTurn = 0, left = 0; int64_t eta = 0; };
bool parseProgress(JsonObjectConst, int currentRoute, NavProgress &out);  // false for another route
class ClipPlan {                                  // which clip to record next, when to play
 public:
  void reset(int route, int count);
  int nextToRecord(int currentStep) const;        // -1: nothing (all ready ≥ 3 ahead, or done)
  void recorded(int i);  bool ready(int i) const;
  enum class Play { None, Clip, Live, Drop };
  Play onProgress(const NavProgress &p, NavMode m, bool busy, const NavRoute &r);  // once per step
};
```

- [ ] **Step 1: Write the failing tests** (`test_nav_route.cpp`):
```cpp
CHECK_EQ("triggers", triggerMetres(NavMode::Walking) * 1000 + triggerMetres(NavMode::Cycling), 40100);
CHECK("route assembles from its steps", r.begin(route3) && !r.complete() && r.step(s0) && r.step(s1) && r.step(s2) && r.complete());
CHECK("a step of another route is refused", !r.step(stepOfRoute2));
CHECK("progress of an old route is ignored", !parseProgress(progressRoute2, 3, p));
CHECK("empty first step: not spoken", !r.at(0).spoken() && r.at(1).spoken());
CHECK("record ahead: the first missing within 3", plan.nextToRecord(1) == 1 && (plan.recorded(1), plan.recorded(2), plan.recorded(3), plan.nextToRecord(1)) == -1);
CHECK("noise at the trigger plays once", plays(45, 38, 42, 35) == 1);          // a helper feeding onProgress
CHECK("not ready → Live", fresh.onProgress(at38step2, NavMode::Walking, false, r) == ClipPlan::Play::Live);
CHECK("busy → None, then Clip when free", plan.onProgress(at38step2, W, true, r) == ClipPlan::Play::None &&
                                          plan.onProgress(at35step2, W, false, r) == ClipPlan::Play::Clip);
CHECK("a passed step's clip is dropped", plan.onProgress(at38step3, W, true, r) == ClipPlan::Play::None &&
                                         plan.onProgress(at300step4, W, false, r) == ClipPlan::Play::Drop);
CHECK("texts in one buffer", r.textBytes() >= 4096);                            // the reserve
```
- [ ] **Step 2:** `python3 tools/companion_test/run.py nav_route` → FAIL (no `nav_route.h`).
- [ ] **Step 3:** Implement. `onProgress`: a step plays when `toTurn ≤ trigger` and it wasn't played; `busy` →
  `None` (asked again next progress); the step changed past it unplayed → `Drop`; not `spoken()` → `None`.
- [ ] **Step 4:** Run → PASS; whole suite `python3 tools/companion_test/run.py` → all pass.
- [ ] **Step 5:** Commit `Directions: the trip on Dotty (nav_route, host-tested)`.

### Task 3: The lines and what the companion knows (`nav_lines.*`, host-tested)

**Files:** Create `cartridges/companion/nav_lines.h`, `nav_lines.cpp`; Test `tools/companion_test/test_nav_lines.cpp`
(uses `nav_lines.cpp nav_route.cpp script_writer.cpp companion_store.cpp`).

**Interfaces:**
- Consumes: `NavRoute`, `NavProgress`, `triggerMetres` (Task 2); `writerRequest`'s style of body (`script_writer.h`), `Profile`.
- Produces:
```cpp
std::string navLinesRequest(const Profile &who, const std::string &memorySection, const NavRoute &r);
std::vector<std::string> parseNavLines(const Written &w, int count);   // "" for a missing line
std::string navStartNote(const NavRoute &r);         // "(Dotty, not the user:) …" confirm + first step
std::string navRerouteNote(const NavRoute &r);
std::string navArrivedNote(const std::string &destination);
std::string navProgressNote(const NavRoute &r, const NavProgress &p);  // with a question during a trip
std::string navSection(const std::vector<std::string> &placeNames, const NavRoute *trip, const NavProgress *p);
struct NearbyPlace { int n; std::string name, category, street, direction, phone; int distance, minutes; };
std::string nearbyResult(const std::string &what, const std::vector<NearbyPlace> &places);  // spoken tool result
std::string nearbyQuery(const std::string &place, const std::vector<NearbyPlace> &last, bool &missing); // "nearby 2" → name; missing = no such result
```
- [ ] **Step 1: Write the failing tests:**
```cpp
CHECK("request: one line per step, instruction first, trigger distance, ≤ 20 words", has(sys, "instruction first") && has(text, "40 metres") && has(sys, "20 words") && has(text, "Rua Augusta"));
CHECK("request: in character with memories", has(sys, who.name) && has(sys, "Likes horses"));
CHECK("parse: a JSON array of strings", parseNavLines(ok3, 3) == lines3);
CHECK("parse: short array padded with \"\"", parseNavLines(ok2, 3)[2].empty());
CHECK("parse: failed write → all empty", parseNavLines(failed, 3) == std::vector<std::string>(3));
CHECK("start note: destination, distance, minutes, first step", has(navStartNote(r), "Rossio") && has(navStartNote(r), "1.4 km") && has(navStartNote(r), "18 minutes"));
CHECK("section: saved places listed", has(navSection({"Home","Mum's"}, nullptr, nullptr), "Saved places: Home, Mum's"));
CHECK("section: transit not possible yet", has(navSection({}, nullptr, nullptr), "public transport"));
CHECK("section: a trip in progress", has(navSection({}, &r, &p), "next: right onto Rua Augusta in 120 m"));
CHECK("nearby result: names, distance, minutes; never 'open'", has(res, "150 m") && has(res, "2 min") && !has(res, "open"));
CHECK("nearby: an urgent hospital → call 112", has(nearbyResult("hospital", hosp), "112"));
CHECK("\"nearby 2\" → the second's name; no results → missing", nearbyQuery("nearby 2", last, m) == "Millennium ATM" && !m && (nearbyQuery("nearby 2", {}, m), m));
```
- [ ] **Step 2:** Run `nav_lines` → FAIL. **Step 3:** Implement (the writer body: `kWriterModel` JSON like
  `writerRequest`, asking for "a JSON array of N strings", `responseMimeType: application/json`).
- [ ] **Step 4:** Run → PASS; whole suite passes. **Step 5:** Commit `Directions: lines and the instruction (host-tested)`.

### Task 4: The tools in the Live setup

**Files:** Modify `cartridges/companion/live_protocol.cpp`, `gemini_live.cpp` (`isMomentTool` += the three);
Test `tools/companion_test/test_live_protocol.cpp`.

- [ ] **Step 1: Failing tests:** the tools list ends `…,new_conversation,navigate_to,stop_navigation,find_nearby,`;
  `navigate_to` requires `place`, `mode` enum `walking|cycling|driving`; `find_nearby` requires `what`; the
  instruction contains `call navigate_to`, `call find_nearby` and `never say a place is open`.
- [ ] **Step 2:** Run `live_protocol` → FAIL. **Step 3:** Declare them; instruction: directions need the iPhone;
  walking unless told; after `find_nearby`, "take me there / the second one" → `navigate_to {place: "nearby N"}`.
- [ ] **Step 4:** Run → PASS. **Step 5:** Commit `Directions: the tools`.

### Task 5: The screens (`nav_screen.*`)

**Files:** Create `cartridges/companion/nav_screen.h`, `nav_screen.cpp`; Modify `screens.h` (forward nothing:
`nav_screen.h` is included by `main.cpp`).

**Produces:**
```cpp
struct NavView { std::string destination, street, clock; Arrow arrow; int exit, toTurn, left, minutes; bool confirm, waiting; };
void drawNav(const NavView &v);            enum class NavHit { None, Stop, Yes, No };  NavHit navHit(uint16_t x, uint16_t y, const NavView &v);
void drawArrow(Adafruit_GFX &g, Arrow a, int exit, int16_t cx, int16_t cy, int16_t size);
int16_t drawNavLockCard(Adafruit_GFX &canvas, const NavView &v);   // the lock widget (200x80): arrow 56 px + distance
struct NearbyRow { std::string name, detail; };                    // "150 m · 2 min" → "150 m - 2 min" (ASCII fonts)
void drawNearby(const std::string &what, const std::vector<NearbyRow> &rows, int page, int pages);
int nearbyHit(uint16_t x, uint16_t y, int rows);                    // -2 back, -3 next page, row, -1 none
```
- [ ] **Step 1:** Implement the layout of the spec's box (nav bar ✕ · destination · arrival; arrow ~96 px left;
  distance FreeSansBold24pt right; street ≤ 2 lines; "1.2 km left - 14 min"); "Waiting for your iPhone..." over
  the street line when `waiting`; the stop question like the recordings' delete question. Arrows: thick strokes
  (3 px), slight 30°, normal 90°, sharp 135°, U-turn, a ring with the exit number, a flag.
- [ ] **Step 2:** Build `pio run -e companion` → SUCCESS. Visual check comes in Task 6 (screenshots).
- [ ] **Step 3:** Commit `Directions: the navigation and nearby screens`.

### Task 6: The trip in the companion (`main.cpp`)

**Files:** Modify `cartridges/companion/main.cpp`, `CLAUDE.md` later (Task 10).

**Consumes:** Tasks 2-5; `LineJob` (`line_job.h`), `pollLines`/`nextLineWork`, `shell::bluetoothWindow`,
`relay::phoneReady`, `ble::notify`, `GeminiLive::answerTool`, `history::append`.

- [ ] **Step 1: Tools.** (`enum class Job` += `NavLines`.) A `navigate_to` during a trip replaces it (the new
  route's clips start over). `navigate_to`: no phone ready → `toolError("directions need your iPhone nearby")`;
  `"nearby N"` resolved with `nearbyQuery` (missing → `toolError("no such place in the last search")`); else
  send `nav.request {req, query, mode}` and hold the call id; answered (spoken, like recall) on `nav.route`
  (`choices` → ask; `!ok` → the error; complete → `navStartNote`), or after 20 s / `count` not reached 3 s after
  `nav.route` → "the route didn't come through". `stop_navigation` → end the trip, `nav.stop`, answered
  silently. `find_nearby` → `nav.nearby`, answered with `nearbyResult` on `nav.places` (+ all `nav.place`), and
  the nearby list opens after the reply (none found → no list); a tap on a row = `navigate_to {place: "nearby N"}`
  through the same path (no Live call: Dotty sends `nav.request` itself and speaks `navStartNote` live), back
  closes it, 2 minutes untouched closes it.
- [ ] **Step 2: Commands** `nav.route`, `nav.step`, `nav.progress`, `nav.arrived`, `nav.ended`, `nav.places`,
  `nav.place`, `companion.places` (names kept in NVS `companion/places`, joined by `\n`, for `navSection`).
- [ ] **Step 3: Clips.** At trip start (and each re-route): `navLinesRequest` → the writer (`Job::Write`-style
  job, `jobKind = Job::NavLines`, not the chat's `writing` flag); lines kept per step; `nextLineWork` first offers
  `ClipPlan::nextToRecord` as a `LineWork {companion, key "nav-<route>-<i>", words 25}` started already
  written (`lineJob.written(given)`, as plan-written lines are); keys go through `moments::recordingPath`, and
  `moments::dropRecording` clears `nav-*` at the trip's end; no card (`storage::available()` false) → no clips,
  every step said live. (Ruling: the spec's `nav/<step>.wav` folder becomes
  the moments line folder with `nav-` keys — one recording pipeline, one cleanup path.)
- [ ] **Step 4: Playing.** `nav.progress` → `ClipPlan::onProgress(…, busy = BOOT held || speaking || a clip
  playing)`: `Clip` → play the WAV; `Live` → `typedTurn`-style note with that step's line (or Apple's text when
  the line is empty); missing line and no session → two beeps (`player.playNotes`). Re-route → `navRerouteNote`
  live at most once a minute; `nav.arrived` → `navArrivedNote`, trip ends. A question during a trip carries
  `navProgressNote` before the user's words.
- [ ] **Step 5: Screen and power.** While a trip runs `drawApp` draws `drawNav` (5 s after a reply ends);
  redraw on ≥ 10 m or a step change, ≤ every 3 s; `shell::wake()` each pass while unlocked; locked →
  `shell::bluetoothWindow(120)` renewed every minute, the lock widget = `drawNavLockCard`; no progress 30 s →
  `waiting`; 2 min → "Lost the iPhone, directions stopped" (note) and the trip ends; power off ends it.
- [ ] **Step 6: Dev key `>`**: a fake 5-step route fed through the same commands (`ble::runHere`),
  progress stepping down 10 m a second, so the screen, clips and live lines run without the phone.
- [ ] **Step 7:** Build, flash (`pio run -e companion -t upload --upload-port /dev/cu.usbmodem1101`), press `>`,
  take screenshots (`.venv/bin/python tools/screenshot.py`) of the navigation screen at 120 m, the stop question,
  a roundabout, the lock card; read the log for "nav: clip 2 played at 38 m". Expected: each clip once, in order.
- [ ] **Step 8:** Commit `Directions: the trip on Dotty`.

### Task 7: Places in the app

**Files:** Create `ios/Dotty/Features/Places/PlacesStore.swift` (model, Application Support/Places/places.json,
`match(_ query: String) -> SavedPlace?`), `PlacesView.swift` (Map, pins, search, sheets), `PlacePin.swift` (the
drawn pin); Modify `CompanionView.swift` (map button beside the clock and wand), `DottyLink.swift`
(`companion.places` on change and each ready connection); Test `tools/relay_swift_check/nav_check.swift`.

**Produces:** `struct SavedPlace: Codable, Identifiable { id: UUID; name: String; icon: PlaceIcon; lat, lon: Double; address: String }`,
`enum PlaceIcon: String, CaseIterable { home, work, heart, star, cup, dumbbell, cart, pin }`,
`PlacesStore.shared.places`, `PlacesStore.match(_:) -> SavedPlace?`.

- [ ] **Step 1: Failing Swift checks** (`nav_check.swift`, run like `recording_check.swift`):
  `match("home")`, `match("my mum's")` → "Mum's", `match("the gym")` → "Gym", `match("Rossio")` → nil.
- [ ] **Step 2:** Run → FAIL (no `PlacesStore`). **Step 3:** Implement matching (lowercased; "my", "the", "'s"
  and articles dropped; exact, then prefix); the screen: `Map` with `Annotation`s using `PlacePin`, the search
  field from `PlacePicker`'s completer, long press → drop pin → save sheet (name, icon), tap → edit/move/delete.
- [ ] **Step 4:** Checks PASS; `xcodebuild … build` → SUCCEEDED. **Step 5:** Commit `App: Places`.

### Task 8: Route geometry and messages (Swift, checked)

**Files:** Create `ios/Dotty/Features/Navigation/RouteGeometry.swift`, `NavMessages.swift`; Test `nav_check.swift`.

**Produces:**
```swift
enum NavMode: String { case walking, cycling, driving }
enum NavArrow: String { case straight, slightLeft = "slight-left", left, sharpLeft = "sharp-left", slightRight = "slight-right", right, sharpRight = "sharp-right", uturn, roundabout, arrive }
struct NavStepInfo { let text, street: String; let distance: Int; let arrow: NavArrow; let exit: Int?; let point: CLLocationCoordinate2D }
enum RouteGeometry {
  static func arrow(turnDegrees: Double, instruction: String) -> (NavArrow, Int?)   // <20 straight, 20-45 slight, 45-135 turn, 135-170 sharp, >170 uturn; "roundabout"/"exit" → exit number
  static func steps(from route: MKRoute) -> [NavStepInfo]                            // drops MapKit's empty first step
  static func metresAlong(_ coords: [CLLocationCoordinate2D], from: CLLocationCoordinate2D, to pointIndex: Int) -> Double
  static func offRoute(distanceFromRoute: Double, mode: NavMode, since: TimeInterval) -> Bool   // 30/50/80 m for 8 s
  static func arrived(distance: Double, mode: NavMode) -> Bool                                  // 25 m / 40 driving
  static func directionWords(from: CLLocationCoordinate2D, to: CLLocationCoordinate2D) -> String // "north of you"
}
enum NavMessages { static func route(…) -> [[String: Any]]  /* nav.route + nav.step each ≤ 512 bytes */ ; static func places(…) -> [[String: Any]] }
```
- [ ] **Step 1: Failing checks:** 90° right → `.right`; 30° left → `.slightLeft`; 175° → `.uturn`; "Take the 2nd
  exit" → (.roundabout, 2); a recorded 5-point track along a polyline → metres to the turn decrease to 0 and the
  step advances; 35 m off for 9 s walking → off; 35 m for 5 s → not; 20 m from the end → arrived; a 40-step route
  → every message's JSON ≤ 512 bytes and `count` = 40.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** PASS. **Step 5:** Commit `App: route geometry and nav messages`.

### Task 9: NavigationService and nearby (app)

**Files:** Create `ios/Dotty/Features/Navigation/NavigationService.swift`, `NearbySearch.swift`,
`NavigationCard.swift`; Modify `ContentView.swift` (attach after `RecordingService`), `DottyLink.swift`
(`nav.request`, `nav.nearby`, `nav.stop` → the service), `CompanionView.swift` (the card),
`RelayLogView.swift` (Simulate walk); Test `nav_check.swift` (category mapping).

**Consumes:** `PlacesStore.match`, `RouteGeometry`, `NavMessages`.
- [ ] **Step 1: Failing checks:** `NearbySearch.category("ATM")` and `("cash machine")` → `.atm`;
  `("hospital")` → `.hospital`; `("a sushi place")` → nil (free text); results sorted nearest first, ≤ 5.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement: resolve (saved → `MKLocalSearch` near you; two similar
  > 300 m apart → `choices`); `MKDirections` by mode (cycling per Task 1's answer); location (best accuracy,
  background, activity type by mode); progress on 10/25/50 m or a step change; off route → new route ≤ every 20 s
  (`reroute: true`, `req: 0`); arrival → `nav.arrived`; Dotty gone 2 min → stop; nearby: POI request filtered by
  category or `MKLocalSearch`, radius 1 km / 5 km, retry ×2 once, ≤ 5, keep the last results; the card "Walking to
  Rossio · 1.2 km · arrive 12:45" + Stop; Simulate walk = feed the route's polyline points at 1.4 m/s as fake fixes.
- [ ] **Step 4:** Checks PASS; build SUCCEEDED. **Step 5:** Commit `App: navigation service and nearby`.

### Task 10: At the desk, docs, version, release

- [ ] **Step 1:** Install the app on the user's iPhone (they run it from Xcode); Dotty 0.8.0 flashed. Simulate
  walk to a saved place: read Dotty's log — route received, clips written and recorded ≥ 3 ahead, each played
  once near 40 m, the arrival line. "Is there an ATM nearby?" → the result spoken and the list on Dotty;
  "take me to the second one" → a trip.
- [ ] **Step 2:** CLAUDE.md: a "Directions" bullet under the companion (tools, messages, numbers, the probe's
  answer, measured times); the roadmap memory note. Bump `DOTTY_CARTRIDGE("companion", "Companion", "0.8.0")`.
- [ ] **Step 3:** Host tests and Swift checks all pass; commit; check outgoing commits for keys
  (`git log -p FETCH_HEAD..HEAD | grep -cE "AIza[0-9A-Za-z_-]{35}"` → 0); push; `.venv/bin/python
  tools/build_catalog.py --release`.
- [ ] **Step 4:** The real walk with the user: logs for clip timing, re-routes and battery (`power: battery` lines).
