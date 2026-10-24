# Companion directions — design

Piece 3c of the companion roadmap, after recordings (0.6.x) and the open conversation (0.7.0). Next: the
outside world through the iPhone (piece 4).

## Goal

Say "Percival, walk me home" or "take me to the nearest pharmacy" and the companion guides you there:
Dotty shows the next turn as a big arrow with the distance, and the companion says every instruction in
its own voice and character ("In about 40 metres, right onto Rua Augusta. Mind the horses, my lord!"),
on time, with the iPhone locked in your pocket.

## User's choices (2026-10-25)

- **Walking first**, also cycling (Dotty on the handlebar) and driving (on the dashboard). **Public
  transport waits** for a later piece: Apple Maps gives apps no step-by-step transit directions; asked for
  a bus route, the companion says it can't plan transit yet and offers to walk you or to open Apple Maps.
- **The companion's voice for everything** — the iPhone's speech synthesis is not good enough, walking
  leaves time for it, and the personality is the point. Made two ways (**option C**): **clips recorded
  ahead** for the planned steps (on time, no tokens or signal needed during the walk) and **Live in real
  time** for the unexpected (the start, re-routes, arrival, your own questions).
- **Destinations**: **saved places** (kept in the app, on an Apple Maps map with our own pins, a search
  field with suggestions as you type) and **any place by search** around you.

## The flow

1. **Asking.** The companion calls `navigate_to {place, mode}`; `mode` = walking unless you say ride /
   cycle / drive.
2. **Finding it (iPhone).** Dotty sends `nav.request`; the app matches a saved place's name first ("home",
   "my mum's" → Mum's), else searches Apple Maps around you (nearest good match first), works out the
   route and sends `nav.route`: destination, distance, arrival time, the steps. Two similar matches far
   apart → `choices`, and the companion asks which. Nothing found or no route → an error the companion
   puts in its own words.
3. **Setting off.** The companion confirms live ("Rossio it is, my lord: 1.4 km, about 18 minutes") and
   says the first instruction. Meanwhile the writer writes every step's line and Live records them into
   clips, always at least 3 steps ahead of you.
4. **On the way.** The iPhone follows your location and sends `nav.progress`; Dotty shows the next turn
   and plays each step's clip at the trigger distance before it.
5. **The unexpected.** Off route → a new route from where you are and a live line ("A detour, my lord!
   Turn back…") plus new clips. Arrival → a live line, then the trip ends. Holding BOOT chats as usual.
6. **Stopping.** "Stop the directions" (`stop_navigation`), ✕ on Dotty's navigation screen, or Stop in
   the app.

Who does what: **iPhone** — place search, routing, live location, step tracking, off-route and arrival
detection. **Dotty** — the screen, clip recording and playback, the live lines. **Text model** (through
the phone, `relay.fetch`) — writes the lines.

## The iPhone side

### Places

A new screen, **Companion › Places** (a map button beside the clock and the wand):

- A full **Apple Maps** map (SwiftUI `Map`) with the saved places as **our own pin**: a teardrop in the
  design system's firefly colours carrying a small icon (home, work, heart, star, cup, dumbbell, …; drawn
  for this piece).
- A **search field** at the top with suggestions as you type (`MKLocalSearchCompleter`, reusing Weather's
  `PlacePicker` code); a suggestion centres the map on it and offers to save it.
- A **long press** drops a pin; saving asks for a name and an icon.
- Tapping a pin: rename, change the icon, move, delete.
- Places are kept in the app (Application Support JSON: id, name, icon, lat, lon, address). Dotty only
  gets the **names** (`companion.places {names}` when they change and on each connection), so the
  instruction can list them ("Saved places: Home, Work, Mum's").

### Navigation service

App-wide, attached at launch like `RelayService` and `RecordingService`:

- **Resolving**: a saved place whose name matches the query (case, "my", "'s" and articles aside) wins;
  else `MKLocalSearch` around the current location; two results with similar relevance more than 300 m
  apart → `choices` (names + distance from you).
- **Route**: `MKDirections` with the mode's transport type (walking, automobile; cycling where MapKit
  offers it on iOS 26 — checked in the probe, else walking routes for cycling).
- **Steps**: Apple's instruction text, the street (from the instruction), the step's distance, and an
  **arrow type**: the bearing change at the maneuver point from the route polyline (in → out segment) →
  straight (< 20°), slight (20-45°), turn (45-135°), sharp (135-170°), U-turn (> 170°), left or right;
  "roundabout" / "exit" in the instruction → roundabout with the exit number; the last step → arrive.
- **Following you**: `CLLocationManager`, best accuracy, `allowsBackgroundLocationUpdates`, activity type
  by mode (fitness / other navigation / automotive), the `location` background mode. Each update →
  current step, metres to its maneuver point **along the route**, distance and time left. `nav.progress`
  goes to Dotty when the metres to the turn change by ≥ 10 m walking (25 cycling, 50 driving), and at
  once on a step change.
- **Off route**: more than 30 m from the route (50 cycling, 80 driving) for 8 s → a new route from here,
  at most every 20 s, sent as `nav.route {reroute: true}`.
- **Arrival**: within 25 m of the destination (40 driving) → `nav.arrived`, the trip ends.
- **Dotty gone**: navigation carries on for 2 minutes waiting for it to reconnect, then stops.
- **App card**: the companion's page shows "Walking to Rossio · 1.2 km · arrive 12:45" with Stop.
- **Permission**: starting location from the background (asked while the iPhone is locked) needs
  **"Always"** location access; the app asks the first time you navigate, explaining why. iOS shows its
  location indicator during a trip. **The plan's first task is a probe** proving a locked iPhone can start
  and keep location updates when Dotty asks.
- **Simulate walk** (a debug row): moves along a real route at walking speed, so the whole chain runs at
  the desk.

### Messages

| Direction | Message | Carries |
| --- | --- | --- |
| Dotty → app | event `nav.request` | `{id, query, mode}` |
| Dotty → app | event `nav.stop` | `{}` |
| App → Dotty | `nav.route` | `{id, ok, error?, choices?, destination, mode, distance, eta, steps, reroute?}`; the steps follow in `nav.step {id, i, text, street, distance, arrow, exit?}` commands (a route passes 512 bytes) |
| App → Dotty | `nav.progress` | `{step, toTurn, left, eta}` |
| App → Dotty | `nav.arrived`, `nav.ended {why}` | |

## Dotty's side

### Tools and instruction

- `navigate_to {place, mode?}` (mode: walking | cycling | driving): answered by Dotty once `nav.route`
  comes back — the destination, distance and time to confirm, `choices` to ask, or the error — as a spoken
  tool result (like recall's).
- `stop_navigation {}`: ends the trip, answered silently.
- The instruction lists the saved places; a session opened during a trip also gets where you're going,
  the mode, the next step and the distance and time left. Within an open session, each question you ask
  on the way goes with a short "(Dotty, not the user:) navigation now: …" note of the current progress,
  so "are we close?" just works. Asked for public transport:
  say transit isn't possible yet, offer to walk or to open Apple Maps on the iPhone.
- Directions need the iPhone: without it connected, `navigate_to` answers "needs your iPhone nearby".

### The navigation screen

It replaces the companion's screen during a trip:

```
┌──────────────────────────┐
│ ✕     Rossio      12:45  │  nav bar: stop · destination · arrival time
├──────────────────────────┤
│   ╔═╗                    │
│   ║ ║═══►      120 m     │  arrow (~96 px) | metres to the turn, bold 24 pt
│   ║ ║                    │
│  Rua Augusta             │  the street you turn onto (≤ 2 lines)
│  1.2 km left · 14 min    │
└──────────────────────────┘
```

- Arrows drawn in code: slight / normal / sharp left and right, straight, U-turn, roundabout with its exit
  number, a flag for the last stretch.
- A partial refresh when the distance changes by ≥ 10 m or the step changes, at most every 3 s (the
  shell's full refresh every 30 partials clears ghosting).
- ✕ asks "Stop the directions?" Yes / No (gone after 10 s).
- Talking: holding BOOT shows the companion's screen; 5 s after the reply ends, back to navigation.

### Clips

- **Writing**: one writer request with every step (Apple's text, street, distance, arrow, the mode's
  trigger distance), the destination, and the companion's character and memories → one line per step,
  JSON array, ≤ ~20 words each, **the instruction first and clear, the flavour after**; the distance said
  is the trigger distance ("in about 40 metres").
- **Trigger distances**: 40 m walking, 100 m cycling, 300 m driving.
- **Recording**: Live reads each line word for word into its own clip (the moments' line recording:
  `AdpcmRecorder` → WAV), in route order, at least 3 steps ahead; a short recording session opens through
  the phone when clips are missing and closes when caught up. Files: the companion's folder `nav/<step>.wav`,
  cleared when the trip ends or re-routes (the new steps get new clips).
- **Playing**: when `toTurn` ≤ the trigger distance, that step's clip plays once. Not ready → the line is
  said live; BOOT held or the companion speaking → it waits; the step already passed → dropped.
- **Live lines**: the start (with the confirmation and the first step), re-routes (at most one spoken per
  minute; otherwise only new clips), arrival — each a short "(Dotty, not the user:)" text turn.

### Power

- No auto-lock during a trip. PWR lock: Bluetooth stays on, clips keep playing, and the lock screen shows
  the next turn (arrow + distance), redrawn on each step change and once a minute. Power off ends the trip.
- No progress for 30 s → "Waiting for your iPhone…"; 2 minutes → "Lost the iPhone, directions stopped".
- Battery while navigating (awake, Bluetooth connected) is measured on a real walk with the 1000 mAh
  battery.

## Failures

| Situation | What happens |
| --- | --- |
| No iPhone connected | "Directions need your iPhone nearby" |
| Location not allowed / only While Using | the app's error; the companion explains; the app prompts for Always |
| Place not found / no route | the companion says so |
| Similar matches far apart | the companion asks which (`choices`) |
| Writer fails, or the Gemini allowance runs out | missing clips are said live if possible, else two beeps at the trigger distance with the arrow |
| No SD card | live lines only |
| GPS jumping in narrow streets | re-routes at most every 20 s; a spoken re-route at most once a minute |

**Privacy**: your location stays on the iPhone; only step instructions, street names and the
destination's name go to Gemini (within the user's red line).

## Out of scope

Public transport; several stops in one trip; offline maps; lane guidance; speed limits and cameras; a map
on Dotty (arrows only).

## Testing

- **Dotty, host-tested plain C++**: route reassembly from pieces, arrow types, trigger distances, which
  clip to record next (≥ 3 ahead), play / wait / drop decisions, the writer request and parsing its
  answer, the instruction's navigation section.
- **iPhone, Swift checks** (like `tools/relay_swift_check`) with recorded GPS tracks: arrows from the
  polyline, metres to the turn along the route, off-route and arrival detection, saved-place matching.
- **At the desk**: a Dotty dev key plays a fake route without the phone (screen, clips); the app's
  Simulate walk runs the whole chain.
- **Probe first**: "Always" location started from the background with the iPhone locked.
- **A real walk with the user**: logs for clip timing, re-routes and battery.

Ships as Companion 0.8.0 plus an app update.
