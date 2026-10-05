import CoreLocation
import MapKit
import SwiftUI

/// The Weather Station cartridge's screen: what Dotty shows now, where the forecast is for
/// (automatic, this iPhone's location, or a searched city) and units (firmware:
/// cartridges/weather/).
struct WeatherView: View {
    @Environment(DottyLink.self) private var link
    @State private var status: Status?
    @State private var error: String?
    @State private var searching = false
    @State private var applying: Source?
    @State private var locator = OneShotLocation()

    /// Where the forecast is for: one of the three, chosen in the Location card.
    enum Source { case automatic, phone, city }

    struct Status {
        var source = Source.automatic
        var placeName = ""
        var imperial = false
        var place = ""
        var temp: Double?
        var feels: Double?
        var humidity: Int?
        var description = ""
        var fetchedAt: Date?
        var fetching = false
        var fetchError: String?
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Weather Station", subtitle: "Today and the next 6 days, from Open-Meteo.")
                        .padding(.top, Spacing.l)
                    NotConnectedNotice()
                    if let error { NoticeCard(kind: .error, text: error) }
                    if let status {
                        nowCard(status).needsDotty(link)
                        locationCard(status).needsDotty(link)
                        settingsCard(status).needsDotty(link)
                    }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await load() }
        }
        .navigationTitle("")
        .task { await load() }
        .onChange(of: link.connection) { _, state in
            if state == .connected { Task { await load() } }
        }
        .onChange(of: link.eventCount) { _, _ in
            if link.lastEvent?.event == "weather.changed" { Task { await load() } }
        }
        .task(id: status?.fetching == true) {
            while status?.fetching == true, !Task.isCancelled {
                try? await Task.sleep(for: .seconds(2))
                await load()
            }
        }
        .sheet(isPresented: $searching) {
            PlacePicker { place in Task { await setLocation(place) } }
                .presentationDetents([.large])
                .presentationBackground(.clear)
        }
    }

    // MARK: - Cards

    private func nowCard(_ s: Status) -> some View {
        GlassCard(title: s.place.isEmpty ? "Now" : s.place) {
            LightRow(title: "Outside", subtitle: s.description.isEmpty ? nil : s.description, systemImage: "cloud.sun") {
                Text(reading(s.temp, s.humidity, imperial: s.imperial))
            }
            LightRow(title: "Feels like", subtitle: "With wind and humidity", systemImage: "thermometer.medium") {
                Text(reading(s.feels, nil, imperial: s.imperial))
            }
            LightRow(title: s.fetching ? "Updating…" : "Forecast",
                     subtitle: s.fetchError.map { "Last try: \($0)" } ?? s.fetchedAt.map { "Updated \($0.formatted(.relative(presentation: .named))) · every hour" },
                     systemImage: "arrow.clockwise") {
                if s.fetching {
                    ProgressView().controlSize(.small).tint(Color.ink)
                } else {
                    Button("Update") { Task { await refresh() } }.buttonStyle(.quiet())
                }
            }
        }
    }

    private func locationCard(_ s: Status) -> some View {
        GlassCard(title: "Location") {
            ChoiceRow(title: "Automatic",
                      subtitle: s.source == .automatic && !s.place.isEmpty ? "\(s.place), found from Dotty's internet"
                                                                         : "Found from Dotty's internet connection",
                      systemImage: "globe", chosen: s.source == .automatic, busy: applying == .automatic) {
                Task { await setAutomatic() }
            }
            ChoiceRow(title: "This iPhone's location",
                      subtitle: s.source == .phone ? "\(s.placeName) · tap to update" : "Sent to Dotty once",
                      systemImage: "location", chosen: s.source == .phone, busy: applying == .phone) {
                Task { await useMyLocation() }
            }
            ChoiceRow(title: s.source == .city ? s.placeName : "A place",
                      subtitle: s.source == .city ? "Tap to choose another" : "Any address or city, on a map",
                      systemImage: "map", chosen: s.source == .city, busy: applying == .city) {
                searching = true
            }
            if s.source == .automatic {
                Text("On your iPhone's hotspot the automatic place can be off by a city; choose one instead.")
                    .font(.lpCaption).foregroundStyle(Color.inkFaint)
                    .padding(.horizontal, Spacing.m).padding(.bottom, Spacing.s)
            }
        }
    }

    private func settingsCard(_ s: Status) -> some View {
        GlassCard(title: "Settings") {
            HStack {
                Text("Units").font(.lpHeadline).foregroundStyle(Color.ink)
                Spacer()
                MorphButton(faces: [MorphFace(light: DottyLight.lagoon.color, title: "°C · km/h"),
                                    MorphFace(light: DottyLight.amber.color, title: "°F · mph")],
                            initial: s.imperial ? 1 : 0) { index in
                    Task { await setUnits(imperial: index == 1) }
                }
                .fixedSize()
            }
            .padding(Spacing.m)
        }
    }

    private func reading(_ temp: Double?, _ humidity: Int?, imperial: Bool) -> String {
        guard let temp else { return "—" }
        let t = String(format: "%.1f°%@", temp, imperial ? "F" : "C")
        return humidity.map { "\(t)  \($0)%" } ?? t
    }

    // MARK: - Dotty

    private func load() async {
        do {
            let r = try await link.send("weather.status")
            var s = Status()
            let location = r["location"] as? [String: Any] ?? [:]
            let automatic = location["automatic"] as? Bool ?? true
            s.source = automatic ? .automatic : (location["source"] as? String) == "phone" ? .phone : .city
            s.placeName = location["name"] as? String ?? ""
            s.imperial = (r["units"] as? String) == "imperial"
            if let now = r["now"] as? [String: Any] {
                s.place = now["place"] as? String ?? ""
                s.temp = now["temp"] as? Double
                s.feels = now["feels"] as? Double
                s.humidity = now["humidity"] as? Int
                s.description = now["description"] as? String ?? ""
                if let at = now["fetchedAt"] as? Double, at > 1_600_000_000 { s.fetchedAt = Self.localDate(at) }
            }
            s.fetching = r["fetching"] as? Bool ?? false
            s.fetchError = r["error"] as? String
            status = s
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    /// Dotty keeps local time as epoch seconds; turn it into a real Date.
    private static func localDate(_ seconds: Double) -> Date {
        let shown = Date(timeIntervalSince1970: seconds)
        return shown.addingTimeInterval(-Double(TimeZone.current.secondsFromGMT(for: shown)))
    }

    private func command(_ name: String, _ args: [String: Any] = [:]) async {
        do {
            try await link.send(name, args)
            await load()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func refresh() async { await command("weather.refresh") }
    private func setUnits(imperial: Bool) async {
        await command("weather.units", ["units": imperial ? "imperial" : "metric"])
    }
    private func setAutomatic() async {
        applying = .automatic
        defer { applying = nil }
        await command("weather.location", ["automatic": true])
    }

    private func setLocation(_ city: City, source: Source = .city) async {
        applying = source
        defer { applying = nil }
        await command("weather.location", ["automatic": false, "lat": city.latitude, "lon": city.longitude,
                                           "name": city.name, "source": source == .phone ? "phone" : "city"])
    }

    private func useMyLocation() async {
        applying = .phone
        defer { applying = nil }
        do {
            let location = try await locator.current()
            let name = await Self.placeName(of: location) ?? "My location"
            await setLocation(City(name: name, detail: "", latitude: location.coordinate.latitude,
                                   longitude: location.coordinate.longitude), source: .phone)
        } catch {
            self.error = "Couldn't get this iPhone's location. Allow Location for Dotty in Settings."
        }
    }

    /// The town at a location ("Lisbon"), so Dotty's title shows a name rather than "My location".
    private static func placeName(of location: CLLocation) async -> String? {
        guard let request = MKReverseGeocodingRequest(location: location),
              let item = try? await request.mapItems.first else { return nil }
        return item.addressRepresentations?.cityName ?? item.name
    }
}

// MARK: - Places

struct City: Identifiable, Hashable {
    var id: String { "\(name)\(latitude)\(longitude)" }
    let name: String
    let detail: String
    let latitude: Double
    let longitude: Double
}

// MARK: - One location reading

/// Asks for "while using" permission once, then one location fix.
@MainActor
final class OneShotLocation: NSObject, @preconcurrency CLLocationManagerDelegate {
    private let manager = CLLocationManager()
    private var continuation: CheckedContinuation<CLLocation, Error>?

    override init() {
        super.init()
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyKilometer  // a city's weather needs no more
    }

    func current() async throws -> CLLocation {
        try await withCheckedThrowingContinuation { continuation in
            self.continuation = continuation
            if manager.authorizationStatus == .notDetermined {
                manager.requestWhenInUseAuthorization()  // locationManagerDidChangeAuthorization continues
            } else {
                manager.requestLocation()
            }
        }
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        guard continuation != nil else { return }
        switch manager.authorizationStatus {
        case .authorizedWhenInUse, .authorizedAlways: manager.requestLocation()
        case .denied, .restricted: finish(.failure(CLError(.denied)))
        default: break
        }
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        if let location = locations.last { finish(.success(location)) }
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        finish(.failure(error))
    }

    private func finish(_ result: Result<CLLocation, Error>) {
        continuation?.resume(with: result)
        continuation = nil
    }
}
