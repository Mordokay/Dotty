import CoreLocation
import Foundation
import MapKit

/// "Is there an ATM nearby?" (directions spec, "What's nearby"): what you asked → an Apple Maps
/// points-of-interest category (or free text), the nearest few, and the walking time. The search itself is
/// in NavigationService; this part is pure for the Swift checks.
enum NearbySearch {
    /// Words people use → the category. nil = search the words as they are ("a sushi place").
    nonisolated static func category(_ what: String) -> MKPointOfInterestCategory? {
        let w = " " + what.lowercased().replacingOccurrences(of: "é", with: "e") + " "
        let table: [(MKPointOfInterestCategory, [String])] = [
            (.atm, ["atm", "cash machine", "cashpoint", "cash point"]),
            (.hospital, ["hospital", "emergency room", "a&e"]),
            (.pharmacy, ["pharmacy", "chemist", "drugstore"]),
            (.cafe, ["coffee", "cafe"]),
            (.bakery, ["bakery", "pastry"]),
            (.restaurant, ["restaurant", "somewhere to eat", "food", "dinner", "lunch"]),
            (.foodMarket, ["supermarket", "grocery", "groceries"]),
            (.gasStation, ["petrol", "gas station", "fuel"]),
            (.evCharger, ["ev charger", "charging station", "charger"]),
            (.parking, ["parking", "car park"]),
            (.park, [" park "]),
            (.museum, ["museum"]),
            (.restroom, ["toilet", "restroom", "bathroom", " wc "]),
            (.postOffice, ["post office"]),
            (.bank, [" bank "]),
            (.police, ["police"]),
        ]
        for (category, words) in table where words.contains(where: { w.contains($0) }) { return category }
        return nil
    }

    /// Nearest first, five at most.
    nonisolated static func nearest<T>(_ items: [T], distance: (T) -> Double) -> [T] {
        Array(items.sorted { distance($0) < distance($1) }.prefix(5))
    }

    /// Straight-line metres → minutes on the way (streets add ~30 %); at least 1.
    nonisolated static func minutes(metres: Double, mode: NavMode) -> Int {
        let speed: Double = mode == .driving ? 8 : mode == .cycling ? 4.5 : 1.35  // m/s, town speeds
        return max(1, Int((metres * 1.3 / speed / 60).rounded()))
    }

    /// A category's name for Dotty ("ATM", "Pharmacy").
    nonisolated static func name(_ category: MKPointOfInterestCategory?) -> String {
        guard let category else { return "" }
        let raw = category.rawValue.replacingOccurrences(of: "MKPOICategory", with: "")
        return raw == "ATM" ? "ATM" : raw.replacingOccurrences(of: "([a-z])([A-Z])", with: "$1 $2", options: .regularExpression)
    }
}
