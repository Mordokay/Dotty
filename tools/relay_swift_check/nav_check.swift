// Swift checks for directions (docs/superpowers/plans/2026-10-25-companion-directions.md): saved-place
// matching (PlacesStore), route geometry and messages (RouteGeometry, NavMessages), nearby categories.
// Run: cat ios/Dotty/Features/Places/PlacesStore.swift tools/relay_swift_check/nav_check.swift > $TMP/n.swift \
//        && swiftc $TMP/n.swift -o $TMP/n && $TMP/n
// (Tasks 8-9 add RouteGeometry.swift, NavMessages.swift and NearbySearch.swift to the cat.)
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

print(failed == 0 ? "ALL PASS" : "\(failed) FAILED")
exit(failed == 0 ? 0 : 1)
