import CoreLocation
import SwiftUI

/// The Weather Station cartridge's screen: what Dotty shows now, where the forecast is for
/// (automatic, this iPhone's location, or a searched city), units, and a correction for the
/// room sensor (firmware: cartridges/weather/).
struct WeatherView: View {
    @Environment(DottyLink.self) private var link
    @State private var status: Status?
    @State private var error: String?
    @State private var searching = false
    @State private var locating = false
    @State private var locator = OneShotLocation()

    struct Status {
        var automatic = true
        var placeName = ""
        var imperial = false
        var insideOffset = 0.0
        var insideTemp: Double?
        var insideHumidity: Int?
        var place = ""
        var temp: Double?
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
                    PageHeader(title: "Weather Station", subtitle: "Outside from Open-Meteo, inside from Dotty's own sensor.")
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
            CitySearchSheet { city in Task { await setLocation(city) } }
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
            LightRow(title: "Inside", subtitle: "Dotty's room sensor", systemImage: "house") {
                Text(reading(s.insideTemp, s.insideHumidity, imperial: s.imperial))
            }
            LightRow(title: s.fetching ? "Updating…" : "Update now",
                     subtitle: s.fetchError.map { "Last try: \($0)" } ?? s.fetchedAt.map { "Updated \($0.formatted(.relative(presentation: .named))) · every hour" },
                     systemImage: "arrow.clockwise",
                     action: s.fetching ? nil : { Task { await refresh() } })
        }
    }

    private func locationCard(_ s: Status) -> some View {
        GlassCard(title: "Location") {
            LightRow(title: "Automatic", subtitle: "From Dotty's internet connection",
                     systemImage: s.automatic ? "checkmark.circle.fill" : "circle",
                     action: { Task { await setAutomatic() } })
            LightRow(title: "This iPhone's location", subtitle: locating ? "Finding you…" : "Exact, sent to Dotty once",
                     systemImage: "location",
                     action: { Task { await useMyLocation() } })
            LightRow(title: s.automatic ? "Choose a city" : s.placeName.isEmpty ? "Chosen place" : s.placeName,
                     subtitle: s.automatic ? "Search by name" : "Tap to choose another",
                     systemImage: s.automatic ? "magnifyingglass" : "checkmark.circle.fill",
                     action: { searching = true })
            if s.automatic {
                Text("On your iPhone's hotspot the automatic place can be off by a city; pick one instead.")
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
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Room sensor").font(.lpHeadline).foregroundStyle(Color.ink)
                    Text("Dotty warms its own sensor a little. Match a thermometer.")
                        .font(.lpCaption).foregroundStyle(Color.inkMuted)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: Spacing.s)
                Stepper(value: Binding(get: { s.insideOffset }, set: { new in Task { await setOffset(new) } }),
                        in: -10...10, step: 0.5) {
                    Text(String(format: "%+.1f°", s.insideOffset)).font(.lpCallout.monospacedDigit())
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
            s.automatic = location["automatic"] as? Bool ?? true
            s.placeName = location["name"] as? String ?? ""
            s.imperial = (r["units"] as? String) == "imperial"
            s.insideOffset = r["insideOffset"] as? Double ?? 0
            if let inside = r["inside"] as? [String: Any] {
                s.insideTemp = inside["temp"] as? Double
                s.insideHumidity = inside["humidity"] as? Int
            }
            if let now = r["now"] as? [String: Any] {
                s.place = now["place"] as? String ?? ""
                s.temp = now["temp"] as? Double
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
    private func setOffset(_ offset: Double) async { await command("weather.inside.offset", ["offset": offset]) }
    private func setAutomatic() async { await command("weather.location", ["automatic": true]) }
    private func setLocation(_ city: City) async {
        await command("weather.location", ["automatic": false, "lat": city.latitude, "lon": city.longitude, "name": city.name])
    }

    private func useMyLocation() async {
        locating = true
        defer { locating = false }
        do {
            let location = try await locator.current()
            await setLocation(City(name: "My location", detail: "", latitude: location.coordinate.latitude,
                                   longitude: location.coordinate.longitude))
        } catch {
            self.error = "Couldn't get this iPhone's location. Allow Location for Dotty in Settings."
        }
    }
}

// MARK: - City search

struct City: Identifiable, Hashable {
    var id: String { "\(name)\(latitude)\(longitude)" }
    let name: String
    let detail: String
    let latitude: Double
    let longitude: Double
}

/// Search a city by name with Open-Meteo's free geocoding (no key), from the phone.
private struct CitySearchSheet: View {
    var onPick: (City) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var query = ""
    @State private var results: [City] = []
    @State private var failed = false
    @FocusState private var focused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: Spacing.l) {
            Text("Choose a city").font(.lpTitle).foregroundStyle(Color.ink)
            TextField("City", text: $query)
                .font(.lpBody)
                .foregroundStyle(Color.ink)
                .focused($focused)
                .autocorrectionDisabled()
                .padding(Spacing.l)
                .glassSurface(cornerRadius: Radius.soft)
            if failed {
                Text("Couldn't search. Check your internet connection.").font(.lpCallout).foregroundStyle(DottyLight.ember.color)
            }
            ScrollView {
                VStack(spacing: 0) {
                    ForEach(results) { city in
                        LightRow(title: city.name, subtitle: city.detail, systemImage: "mappin.and.ellipse",
                                 action: { onPick(city); dismiss() })
                    }
                }
            }
            Button("Cancel") { dismiss() }.buttonStyle(.quiet())
        }
        .padding(Spacing.xl)
        .glassSurface(cornerRadius: Radius.sheet, frosted: true)
        .padding(Spacing.s)
        .onAppear { focused = true }
        .task(id: query) {
            let text = query.trimmingCharacters(in: .whitespaces)
            guard text.count >= 2 else { results = []; return }
            try? await Task.sleep(for: .milliseconds(300))  // wait for typing to pause
            guard !Task.isCancelled else { return }
            await search(text)
        }
    }

    private func search(_ text: String) async {
        var components = URLComponents(string: "https://geocoding-api.open-meteo.com/v1/search")!
        components.queryItems = [URLQueryItem(name: "name", value: text), URLQueryItem(name: "count", value: "8")]
        do {
            let (data, _) = try await URLSession.shared.data(from: components.url!)
            let json = try JSONSerialization.jsonObject(with: data) as? [String: Any]
            results = (json?["results"] as? [[String: Any]] ?? []).compactMap { r in
                guard let name = r["name"] as? String, let lat = r["latitude"] as? Double,
                      let lon = r["longitude"] as? Double else { return nil }
                let detail = [r["admin1"] as? String, r["country"] as? String].compactMap { $0 }.joined(separator: ", ")
                return City(name: name, detail: detail, latitude: lat, longitude: lon)
            }
            failed = false
        } catch {
            failed = true
        }
    }
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
