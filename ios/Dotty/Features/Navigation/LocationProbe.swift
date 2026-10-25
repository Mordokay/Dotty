import CoreLocation
import MapKit
import UIKit

/// A probe (directions plan, Task 1): can a locked iPhone, woken by a Dotty event (`nav.probe`), start
/// location updates and keep them for 2 minutes? Directions start that way ("walk me home" with the
/// iPhone in a pocket), which needs "Always" access. Also: does MapKit give cycling routes on this iOS?
/// Logs to the Relay log.
@MainActor
final class LocationProbe: NSObject {
    static let shared = LocationProbe()

    private let manager = CLLocationManager()
    private var fixes = 0
    private var best = Double.infinity
    private var running = false
    private var last: CLLocation?
    private var log: (String) -> Void = { RelayService.shared.log($0) }

    override init() {
        super.init()
        manager.delegate = self
    }

    /// In the foreground: ask for "Always" (iOS asks for While Using first, then offers Always).
    func askAlways() {
        switch manager.authorizationStatus {
        case .notDetermined: manager.requestWhenInUseAuthorization()
        default: manager.requestAlwaysAuthorization()
        }
        log("location probe: authorization \(Self.name(manager.authorizationStatus)); asked for Always")
    }

    func run() {
        guard !running else { return log("location probe: already running") }
        running = true
        fixes = 0
        best = .infinity
        let state = UIApplication.shared.applicationState == .active ? "front" : "background"
        log("location probe: start [\(state)], authorization \(Self.name(manager.authorizationStatus))")
        manager.desiredAccuracy = kCLLocationAccuracyBest
        manager.activityType = .fitness
        manager.allowsBackgroundLocationUpdates = true
        manager.showsBackgroundLocationIndicator = true
        manager.startUpdatingLocation()
        Task {
            for second in stride(from: 15, through: 120, by: 15) {
                try? await Task.sleep(nanoseconds: 15_000_000_000)
                let state = UIApplication.shared.applicationState == .active ? "front" : "background"
                log(String(format: "location probe %d s [%@]: %d fixes, best ±%.0f m", second, state, fixes,
                           best.isFinite ? best : -1))
            }
            manager.stopUpdatingLocation()
            running = false
            if let here = last?.coordinate { await routes(from: here) }
            log("location probe: done, \(fixes) fixes in 2 minutes")
        }
    }

    /// A walking and a cycling route to a point 1 km north.
    private func routes(from here: CLLocationCoordinate2D) async {
        let there = CLLocationCoordinate2D(latitude: here.latitude + 0.009, longitude: here.longitude)
        for (name, type) in [("walking", MKDirectionsTransportType.walking), ("cycling", MKDirectionsTransportType.cycling)] {
            let request = MKDirections.Request()
            request.source = MKMapItem(placemark: MKPlacemark(coordinate: here))
            request.destination = MKMapItem(placemark: MKPlacemark(coordinate: there))
            request.transportType = type
            do {
                let answer = try await MKDirections(request: request).calculate()
                let route = answer.routes.first
                log("location probe: \(name) route \(Int(route?.distance ?? 0)) m, \(route?.steps.count ?? 0) steps")
            } catch {
                log("location probe: \(name) route failed: \(error.localizedDescription)")
            }
        }
    }

    fileprivate func got(_ locations: [CLLocation]) {
        fixes += locations.count
        for l in locations where l.horizontalAccuracy >= 0 { best = min(best, l.horizontalAccuracy) }
        last = locations.last ?? last
    }

    fileprivate func failed(_ error: Error) {
        log("location probe error: \(error.localizedDescription)")
    }

    static func name(_ s: CLAuthorizationStatus) -> String {
        switch s {
        case .authorizedAlways: return "always"
        case .authorizedWhenInUse: return "while using"
        case .denied: return "denied"
        case .restricted: return "restricted"
        case .notDetermined: return "not asked"
        @unknown default: return "unknown"
        }
    }
}

// CoreLocation calls these on the thread the manager was made on; they're nonisolated and hop to the main
// actor (a MainActor closure called off main traps under Swift 6: the SpeechAuth launch crash).
extension LocationProbe: CLLocationManagerDelegate {
    nonisolated func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        Task { @MainActor in LocationProbe.shared.got(locations) }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        Task { @MainActor in LocationProbe.shared.failed(error) }
    }

    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        let status = manager.authorizationStatus
        Task { @MainActor in RelayService.shared.log("location probe: authorization now \(LocationProbe.name(status))") }
    }
}
