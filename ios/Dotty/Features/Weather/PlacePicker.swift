import MapKit
import SwiftUI

/// Pick the forecast's place with Apple Maps: suggestions update as you type (no Return
/// needed), and the map follows the best match live; tap another suggestion to move it there.
struct PlacePicker: View {
    var onPick: (City) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var search = PlaceSearch()
    @State private var query = ""
    @State private var chosen: MKLocalSearchCompletion?
    @State private var tapped = false  // a tapped suggestion stays until the text changes
    @State private var place: City?
    @State private var camera: MapCameraPosition = .automatic
    @State private var failed = false
    @FocusState private var focused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: Spacing.l) {
            Text("Choose a place").font(.lpTitle).foregroundStyle(Color.ink)
            TextField("Address or city", text: $query)
                .font(.lpBody)
                .foregroundStyle(Color.ink)
                .focused($focused)
                .autocorrectionDisabled()
                .submitLabel(.done)
                .padding(Spacing.l)
                .glassSurface(cornerRadius: Radius.soft)
            Map(position: $camera) {
                if let place {
                    Marker(place.name, coordinate: CLLocationCoordinate2D(latitude: place.latitude, longitude: place.longitude))
                }
            }
            .mapStyle(.standard(pointsOfInterest: .excludingAll))
            .frame(height: 200)
            .clipShape(RoundedRectangle(cornerRadius: Radius.soft, style: .continuous))
            if failed {
                Text("Couldn't find that place. Check your internet connection.")
                    .font(.lpCallout).foregroundStyle(DottyLight.ember.color)
            }
            ScrollView {
                VStack(spacing: 0) {
                    ForEach(search.results, id: \.self) { result in
                        ChoiceRow(title: result.title, subtitle: result.subtitle.isEmpty ? nil : result.subtitle,
                                  chosen: result == chosen) {
                            focused = false
                            tapped = true
                            Task { await show(result) }
                        }
                    }
                }
            }
            .scrollDismissesKeyboard(.immediately)
            HStack {
                Button("Cancel") { dismiss() }.buttonStyle(.quiet())
                Spacer()
                Button(place.map { "Use \($0.name)" } ?? "Use this place") {
                    if let place { onPick(place); dismiss() }
                }
                .buttonStyle(.light())
                .disabled(place == nil)
                .opacity(place == nil ? 0.4 : 1)
            }
        }
        .padding(Spacing.xl)
        .glassSurface(cornerRadius: Radius.sheet, frosted: true)
        .padding(Spacing.s)
        .onAppear { focused = true }
        .onChange(of: query) { _, text in
            tapped = false
            search.update(text)
        }
        // The best match follows the typing; wait for a short pause so the map doesn't jump per letter.
        .task(id: search.results.first) {
            guard let best = search.results.first, !tapped else { return }
            try? await Task.sleep(for: .milliseconds(350))
            guard !Task.isCancelled else { return }
            await show(best)
        }
    }

    private func show(_ result: MKLocalSearchCompletion) async {
        chosen = result
        guard let item = try? await MKLocalSearch(request: MKLocalSearch.Request(completion: result)).start().mapItems.first
        else {
            failed = true
            return
        }
        failed = false
        let coordinate = item.location.coordinate
        // Dotty's title shows the town; the forecast uses the exact coordinates.
        let name = item.addressRepresentations?.cityName ?? item.name ?? result.title
        place = City(name: name, detail: result.subtitle, latitude: coordinate.latitude, longitude: coordinate.longitude)
        withAnimation(.settle) {
            camera = .region(MKCoordinateRegion(center: coordinate, latitudinalMeters: 4000, longitudinalMeters: 4000))
        }
    }
}

/// MapKit's type-ahead search: `results` follow `update(_:)` as the user types.
@MainActor @Observable
private final class PlaceSearch: NSObject, @preconcurrency MKLocalSearchCompleterDelegate {
    private(set) var results: [MKLocalSearchCompletion] = []
    @ObservationIgnored private let completer = MKLocalSearchCompleter()

    override init() {
        super.init()
        completer.delegate = self
        completer.resultTypes = [.address, .pointOfInterest]
    }

    func update(_ text: String) {
        let trimmed = text.trimmingCharacters(in: .whitespaces)
        if trimmed.count < 2 {
            completer.cancel()
            results = []
        } else {
            completer.queryFragment = trimmed
        }
    }

    func completerDidUpdateResults(_ completer: MKLocalSearchCompleter) {
        results = Array(completer.results.prefix(8))
    }

    func completer(_ completer: MKLocalSearchCompleter, didFailWithError error: Error) {
        results = []
    }
}
