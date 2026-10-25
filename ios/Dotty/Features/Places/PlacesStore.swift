import Foundation
import Observation

/// A place saved in the app for directions ("walk me home"): kept on this iPhone only; Dotty gets the
/// names (companion.places) so the companion knows them.
struct SavedPlace: Codable, Identifiable, Hashable, Sendable {
    var id = UUID()
    var name: String
    var icon: PlaceIcon
    var latitude: Double
    var longitude: Double
    var address: String
}

/// The small picture on a place's pin (an SF Symbol in the app).
enum PlaceIcon: String, Codable, CaseIterable, Sendable {
    case home, work, heart, star, cup, dumbbell, cart, pin

    var symbol: String {
        switch self {
        case .home: "house.fill"
        case .work: "briefcase.fill"
        case .heart: "heart.fill"
        case .star: "star.fill"
        case .cup: "cup.and.saucer.fill"
        case .dumbbell: "dumbbell.fill"
        case .cart: "cart.fill"
        case .pin: "mappin"
        }
    }
}

/// The saved places, in Application Support/Places/places.json.
@MainActor @Observable
final class PlacesStore {
    static let shared = PlacesStore()

    private(set) var places: [SavedPlace] = []
    /// Told whenever the list changes (NavigationService sends the names to Dotty).
    @ObservationIgnored var onChange: (([String]) -> Void)?

    private let url: URL = {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Places", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("places.json")
    }()

    init() {
        if let data = try? Data(contentsOf: url), let list = try? JSONDecoder().decode([SavedPlace].self, from: data) {
            places = list
        }
    }

    var names: [String] { places.map(\.name) }

    func save(_ place: SavedPlace) {
        if let i = places.firstIndex(where: { $0.id == place.id }) { places[i] = place } else { places.append(place) }
        persist()
    }

    func remove(_ id: UUID) {
        places.removeAll { $0.id == id }
        persist()
    }

    func match(_ query: String) -> SavedPlace? { Self.match(query, in: places) }

    /// "home", "my mum's", "the gym" → that saved place: case, "my", "the", "our", "'s" and spaces aside;
    /// the whole name first, then a name the words start.
    nonisolated static func match(_ query: String, in places: [SavedPlace]) -> SavedPlace? {
        let q = simplify(query)
        guard !q.isEmpty else { return nil }
        if let exact = places.first(where: { simplify($0.name) == q }) { return exact }
        return places.first { place in
            let n = simplify(place.name)
            return !n.isEmpty && (q.hasPrefix(n + " ") || n.hasPrefix(q + " "))
        }
    }

    nonisolated static func simplify(_ s: String) -> String {
        var words = s.lowercased()
            .replacingOccurrences(of: "’", with: "'")
            .replacingOccurrences(of: "'s", with: "")
            .replacingOccurrences(of: "'", with: "")
            .split(whereSeparator: { !$0.isLetter && !$0.isNumber })
            .map(String.init)
        words.removeAll { ["my", "the", "our", "to", "a"].contains($0) }
        return words.joined(separator: " ")
    }

    private func persist() {
        if let data = try? JSONEncoder().encode(places) { try? data.write(to: url, options: .atomic) }
        onChange?(names)
    }
}
