// Swift checks for directions (docs/superpowers/plans/2026-10-25-companion-directions.md): saved-place
// matching (PlacesStore), route geometry and messages (RouteGeometry, NavMessages), nearby categories.
// Run: cat ios/Dotty/Features/Places/PlacesStore.swift tools/relay_swift_check/nav_check.swift > $TMP/n.swift \
//        && swiftc $TMP/n.swift -o $TMP/n && $TMP/n
// (Tasks 8-9 add RouteGeometry.swift, NavMessages.swift and NearbySearch.swift to the cat.)
import CoreLocation
import MapKit
import Foundation

var failed = 0
func check(_ name: String, _ ok: Bool) {
    print(ok ? "PASS \(name)" : "FAIL \(name)")
    if !ok { failed += 1 }
}

let saved = [
    SavedPlace(name: "Home", icon: .home, latitude: 38.71, longitude: -9.14, address: "Rua A"),
    SavedPlace(name: "Mum's", icon: .heart, latitude: 38.72, longitude: -9.15, address: "Rua B"),
    SavedPlace(name: "Gym", icon: .dumbbell, latitude: 38.73, longitude: -9.16, address: "Rua C"),
]
check("match: home", PlacesStore.match("home", in: saved)?.name == "Home")
check("match: my mum's → Mum's", PlacesStore.match("my mum's", in: saved)?.name == "Mum's")
check("match: my mum → Mum's", PlacesStore.match("My Mum", in: saved)?.name == "Mum's")
check("match: the gym → Gym", PlacesStore.match("the gym", in: saved)?.name == "Gym")
check("match: Rossio → nil (a search)", PlacesStore.match("Rossio", in: saved) == nil)
check("match: empty → nil", PlacesStore.match("  ", in: saved) == nil)

// Route geometry (Task 8)
check("arrow: 90° right", RouteGeometry.arrow(turnDegrees: 90, instruction: "Turn right").0 == .right)
check("arrow: 30° left", RouteGeometry.arrow(turnDegrees: -30, instruction: "Bear left").0 == .slightLeft)
check("arrow: 10° straight", RouteGeometry.arrow(turnDegrees: 10, instruction: "Continue").0 == .straight)
check("arrow: 150° sharp right", RouteGeometry.arrow(turnDegrees: 150, instruction: "Turn sharply right").0 == .sharpRight)
check("arrow: 175° U-turn", RouteGeometry.arrow(turnDegrees: 175, instruction: "Make a U-turn").0 == .uturn)
let round = RouteGeometry.arrow(turnDegrees: 40, instruction: "At the roundabout, take the 2nd exit onto Rua do Ouro")
check("arrow: roundabout, exit 2", round.0 == .roundabout && round.1 == 2)
check("arrow: third exit in words", RouteGeometry.arrow(turnDegrees: 0, instruction: "Take the third exit").1 == 3)
check("street from an instruction", RouteGeometry.street("Turn right onto Rua Augusta") == "Rua Augusta"
      && RouteGeometry.street("Continue on Avenida da Liberdade") == "Avenida da Liberdade"
      && RouteGeometry.street("Arrive at Rossio") == "")

// A straight street north 300 m, then east 200 m (about 0.0027° of latitude per 300 m).
let a = CLLocationCoordinate2D(latitude: 38.7100, longitude: -9.1400)
let b = CLLocationCoordinate2D(latitude: 38.7127, longitude: -9.1400)
let c = CLLocationCoordinate2D(latitude: 38.7127, longitude: -9.1377)
let line = [a, b, c]
check("turn at b is right (~90°)", abs(RouteGeometry.turnDegrees(a, b, c) - 90) < 5)
let start = RouteGeometry.metresAlong(line, from: a, to: 1)
let halfway = RouteGeometry.metresAlong(line, from: CLLocationCoordinate2D(latitude: 38.71135, longitude: -9.1400), to: 1)
check("metres to the turn shrink along the street", abs(start - 300) < 10 && abs(halfway - 150) < 10)
check("a point beside the street is near it", RouteGeometry.distanceToRoute(line, CLLocationCoordinate2D(latitude: 38.71135, longitude: -9.13970)) < 30)
check("a point a street away is off it", RouteGeometry.distanceToRoute(line, CLLocationCoordinate2D(latitude: 38.71135, longitude: -9.1395)) > 35)
check("off route: 35 m for 9 s walking", RouteGeometry.offRoute(distanceFromRoute: 35, mode: .walking, since: 9))
check("not yet: 35 m for 5 s", !RouteGeometry.offRoute(distanceFromRoute: 35, mode: .walking, since: 5))
check("driving allows 80 m", !RouteGeometry.offRoute(distanceFromRoute: 60, mode: .driving, since: 20))
check("arrived: 20 m walking, not 30", RouteGeometry.arrived(distance: 20, mode: .walking) && !RouteGeometry.arrived(distance: 30, mode: .walking))
check("arrived: 35 m driving", RouteGeometry.arrived(distance: 35, mode: .driving))
check("direction in words", RouteGeometry.directionWords(from: a, to: b) == "north of you" && RouteGeometry.directionWords(from: b, to: c) == "east of you")
check("progress step: drops the empty first step", RouteGeometry.firstSpoken(["", "Turn right"]) == 1)

// Messages (Task 8): every command fits 512 bytes, the steps follow the route.
let long = String(repeating: "Turn right onto Rua de São Julião da Praça do Comércio ", count: 8)
var infos: [NavStepInfo] = [NavStepInfo(text: "", street: "", distance: 0, arrow: .straight, exit: nil, point: a)]
for i in 1..<40 { infos.append(NavStepInfo(text: i == 7 ? long : "Turn right onto Rua \(i)", street: "Rua \(i)", distance: 120, arrow: .right, exit: nil, point: b)) }
let msgs = NavMessages.route(req: 3, route: 9, destination: "Rossio", mode: .walking, distance: 1400, eta: 1_800_000_000, steps: infos, reroute: false)
let sizes = msgs.map { (try? JSONSerialization.data(withJSONObject: $0).count) ?? 9999 }
check("route: one header + 40 steps", msgs.count == 41 && (msgs[0]["count"] as? Int) == 40 && (msgs[0]["cmd"] as? String) == "nav.route")
check("route: every command ≤ 512 bytes", sizes.allSatisfy { $0 <= 512 })
check("route: steps carry the route number and arrow", (msgs[2]["route"] as? Int) == 9 && (msgs[2]["arrow"] as? String) == "right")

// Review fixes: a keepalive while standing still; arriving where the route ends short of the place
check("progress: a step change at once; 2 m walking, at most once a second (the user: an update every 1-2 s)",
      RouteGeometry.progressDue(stepChanged: true, metresChanged: 0, mode: .walking, sinceLast: 0.2)
      && RouteGeometry.progressDue(stepChanged: false, metresChanged: 3, mode: .walking, sinceLast: 1.2)
      && !RouteGeometry.progressDue(stepChanged: false, metresChanged: 3, mode: .walking, sinceLast: 0.5)
      && !RouteGeometry.progressDue(stepChanged: false, metresChanged: 1, mode: .walking, sinceLast: 5))
check("progress: 10 m driving", !RouteGeometry.progressDue(stepChanged: false, metresChanged: 6, mode: .driving, sinceLast: 2)
      && RouteGeometry.progressDue(stepChanged: false, metresChanged: 11, mode: .driving, sinceLast: 2))
check("progress: every 15 s while standing still", RouteGeometry.progressDue(stepChanged: false, metresChanged: 0, mode: .walking, sinceLast: 15))
check("arrived where the route ends, the place set back", RouteGeometry.arrived(toTarget: 60, leftAlong: 15, mode: .walking)
      && !RouteGeometry.arrived(toTarget: 60, leftAlong: 120, mode: .walking) && RouteGeometry.arrived(toTarget: 20, leftAlong: 300, mode: .walking))
check("no re-route near the end", !RouteGeometry.rerouteAllowed(leftAlong: 80) && RouteGeometry.rerouteAllowed(leftAlong: 400))

// Nearby (Task 9): what you asked → a points-of-interest category, or free text
check("nearby: ATM / cash machine → atm", NearbySearch.category("ATM") == .atm && NearbySearch.category("a cash machine") == .atm)
check("nearby: hospital", NearbySearch.category("the nearest hospital") == .hospital)
check("nearby: coffee → cafe", NearbySearch.category("good coffee") == .cafe)
check("nearby: toilet → restroom", NearbySearch.category("a toilet") == .restroom)
check("nearby: a sushi place → free text", NearbySearch.category("a sushi place") == nil)
let picked = NearbySearch.nearest([("C", 900.0), ("A", 120.0), ("B", 450.0), ("D", 1000.0), ("E", 50.0), ("F", 2000.0)], distance: { $0.1 })
check("nearby: nearest first, at most 5", picked.map(\.0) == ["E", "A", "B", "C", "D"])
check("nearby: walking minutes", NearbySearch.minutes(metres: 150, mode: .walking) == 2 && NearbySearch.minutes(metres: 10, mode: .walking) == 1)

print(failed == 0 ? "ALL PASS" : "\(failed) FAILED")
exit(failed == 0 ? 0 : 1)
