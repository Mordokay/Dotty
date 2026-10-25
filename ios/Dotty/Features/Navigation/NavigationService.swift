import CoreLocation
import MapKit
import UIKit

/// Directions for Dotty (directions spec): finds the place (a saved one, else Apple Maps around you), routes,
/// follows you with the iPhone locked, and tells Dotty the steps and your progress (nav.route / nav.step /
/// nav.progress / nav.arrived / nav.ended); answers "what's nearby" (nav.places / nav.place). App-wide,
/// attached at launch like RelayService. Dotty holds the screen and the voice.
@MainActor @Observable
final class NavigationService: NSObject {
    static let shared = NavigationService()

    struct Trip {
        var route: Int
        var mode: NavMode
        var destination: String
        var target: CLLocationCoordinate2D
        var line: [CLLocationCoordinate2D]  // the whole route
        var turns: [Int]                    // each step's turn: its index in `line`
        var steps: [NavStepInfo]
        var totalTime: TimeInterval
        var totalDistance: Double
        var step = 0, sentToTurn = -1000
        var sentAt = Date.distantPast
        var offSince: Date?
        var lastReroute = Date.distantPast
        var then: String?  // a stop on the way: where to go on to after arriving
        var ready = false  // all its steps are with Dotty: progress may go (before, Dotty can't place it)
        var left: Double = 0
        var eta = Date()
    }

    private(set) var trip: Trip?
    /// Moves along the route at walking speed instead of using GPS (Relay log › Simulate walk).
    var simulate = false

    @ObservationIgnored private weak var link: DottyLink?
    @ObservationIgnored private let manager = CLLocationManager()
    @ObservationIgnored private var here: CLLocation?
    @ObservationIgnored private var waitingForFix: [CheckedContinuation<CLLocation?, Never>] = []
    @ObservationIgnored private var routeCounter = Int(Date().timeIntervalSince1970) % 100_000
    @ObservationIgnored private var lastNearby: [MKMapItem] = []
    @ObservationIgnored private var disconnectedSince: Date?
    @ObservationIgnored private var simulation: Task<Void, Never>?
    @ObservationIgnored private var abandoned: Set<Int> = []  // requests Dotty gave up on (nav.stop {req})

    override init() {
        super.init()
        manager.delegate = self
    }

    func attach(_ link: DottyLink) {
        self.link = link
        link.onNavEvent = { [weak self] message in self?.event(message) }
        let previous = link.onReady
        link.onReady = { [weak self] in
            previous?()
            self?.sendPlaceNames()
            self?.disconnectedSince = nil
        }
        PlacesStore.shared.onChange = { [weak self] _ in self?.sendPlaceNames() }
        Task { [weak self] in  // Dotty gone for 2 minutes: the trip stops
            while true {
                try? await Task.sleep(for: .seconds(10))
                guard let self else { return }
                self.watchConnection()
            }
        }
    }

    private func log(_ s: String) { RelayService.shared.log("nav: " + s) }

    private func sendPlaceNames() {
        guard let link, link.connection == .connected else { return }
        let names = Array(PlacesStore.shared.names.prefix(20))
        Task { try? await link.send("companion.places", ["names": names]) }
    }

    private func watchConnection() {
        guard trip != nil, let link else { return }
        if link.connection == .connected { disconnectedSince = nil; return }
        if disconnectedSince == nil { disconnectedSince = Date() }
        if let since = disconnectedSince, Date().timeIntervalSince(since) > 120 { stop(why: "Dotty went away") }
    }

    // MARK: - Dotty's events

    private func event(_ m: DottyMessage) {
        let req = m["req"] as? Int ?? 0
        let mode = NavMode(rawValue: m["mode"] as? String ?? "walking") ?? .walking
        switch m.event {
        case "nav.request":
            let then = (m["then"] as? String).flatMap { $0.isEmpty ? nil : $0 }
            Task { await request(req: req, query: m["query"] as? String ?? "", mode: mode, then: then) }
        case "nav.nearby": Task { await nearby(req: req, what: m["what"] as? String ?? "", mode: mode) }
        case "nav.stop":
            if req > 0 { abandoned.insert(req) }  // a request still being worked out: don't start it
            stop(why: nil)
        default: break
        }
    }

    // MARK: - Finding the place and the way

    private enum Resolved {
        case found(MKMapItem, String)
        case choices([(name: String, distance: Int)])
        case failed(String)
    }

    /// A saved place, a find_nearby result ("nearby 2"), else Apple Maps around you (asking which one when
    /// two with the same name are far apart, unless `allowChoices` is off: then the nearest).
    private func resolve(_ query: String, from: CLLocation, allowChoices: Bool) async -> Resolved {
        let lower = query.lowercased()
        if lower.hasPrefix("nearby "), let n = Int(lower.dropFirst(7)) {  // a find_nearby result
            guard n >= 1, n <= lastNearby.count else { return .failed("that nearby place isn't known any more: search again") }
            return .found(lastNearby[n - 1], lastNearby[n - 1].name ?? query)
        }
        if let saved = PlacesStore.shared.match(query) {
            return .found(MKMapItem(location: CLLocation(latitude: saved.latitude, longitude: saved.longitude), address: nil), saved.name)
        }
        let search = MKLocalSearch.Request()
        search.naturalLanguageQuery = query
        search.region = MKCoordinateRegion(center: from.coordinate, latitudinalMeters: 40_000, longitudinalMeters: 40_000)
        guard let items = try? await MKLocalSearch(request: search).start().mapItems, !items.isEmpty else {
            return .failed("nothing called \"\(query)\" was found nearby")
        }
        let nearestWanted = !allowChoices || ["nearest", "closest", "nearby", "near me"].contains { lower.contains($0) }
        let sameName = items.filter { ($0.name ?? "").lowercased() == (items[0].name ?? "").lowercased() }
        let spread = sameName.count >= 2 && sameName[0].location.distance(from: sameName[1].location) > 300
        if spread && !nearestWanted {  // which one? the companion asks
            return .choices(NearbySearch.nearest(sameName, distance: { $0.location.distance(from: from) })
                .prefix(3).map { (name: ($0.name ?? query) + (street($0).isEmpty ? "" : " on " + street($0)),
                                 distance: Int($0.location.distance(from: from))) })
        }
        let target = nearestWanted ? NearbySearch.nearest(items, distance: { $0.location.distance(from: from) })[0] : items[0]
        return .found(target, target.name ?? query)
    }

    /// Dotty asks: a route to `query`; `then` = where to go on to after it (a stop on the way).
    private func request(req: Int, query: String, mode: NavMode, then: String?) async {
        log("request \(req): \"\(query)\" (\(mode.rawValue))" + (then.map { ", then \"\($0)\"" } ?? ""))
        let background = UIApplication.shared.beginBackgroundTask(withName: "nav-request")
        defer { UIApplication.shared.endBackgroundTask(background) }
        guard authorized() else {
            return await fail(req, "location isn't allowed for the Dotty app: it needs \"Always\" (Settings › Dotty › Location)")
        }
        guard let from = await fix() else { return await fail(req, "the iPhone couldn't tell where it is") }
        switch await resolve(query, from: from, allowChoices: true) {
        case .failed(let why): return await fail(req, why)
        case .choices(let choices): return await send([NavMessages.failure(req: req, route: nextRoute(), error: nil, choices: choices)])
        case .found(let target, let name):
            guard let route = await directions(from: from.coordinate, to: target, mode: mode) else {
                return await fail(req, "no \(mode.rawValue) route was found to \(name)")
            }
            guard !abandoned.contains(req) else { return log("request \(req) was given up on by Dotty") }
            start(route: route, req: req, name: name, target: target.location.coordinate, mode: mode, reroute: false,
                  then: then, continuing: false)
        }
    }

    /// Arrived at a stop on the way: the next leg, from here, on its own (Dotty waits for it: continue).
    private func continueTo(_ next: String, mode: NavMode) async {
        let background = UIApplication.shared.beginBackgroundTask(withName: "nav-continue")
        defer { UIApplication.shared.endBackgroundTask(background) }
        guard let from = await fix() else { return await failNext("the iPhone couldn't tell where it is") }
        guard case .found(let target, let name) = await resolve(next, from: from, allowChoices: false) else {
            return await failNext("\(next) couldn't be found")
        }
        guard let route = await directions(from: from.coordinate, to: target, mode: mode) else {
            return await failNext("no \(mode.rawValue) route was found to \(name)")
        }
        start(route: route, req: 0, name: name, target: target.location.coordinate, mode: mode, reroute: false, then: nil,
              continuing: true)
    }

    private func failNext(_ why: String) async {
        log("next leg failed: \(why)")
        var m = NavMessages.failure(req: 0, route: nextRoute(), error: why, choices: [])
        m["continue"] = true
        await send([m])
    }

    private func directions(from: CLLocationCoordinate2D, to: MKMapItem, mode: NavMode) async -> MKRoute? {
        let request = MKDirections.Request()
        request.source = MKMapItem(location: CLLocation(latitude: from.latitude, longitude: from.longitude), address: nil)
        request.destination = to
        request.transportType = mode == .driving ? .automobile : mode == .cycling ? .cycling : .walking
        if let route = try? await MKDirections(request: request).calculate().routes.first { return route }
        if mode == .cycling {  // no cycling routes here: a walking one
            request.transportType = .walking
            return try? await MKDirections(request: request).calculate().routes.first
        }
        return nil
    }

    private func nextRoute() -> Int {
        routeCounter += 1
        return routeCounter
    }

    private func start(route: MKRoute, req: Int, name: String, target: CLLocationCoordinate2D, mode: NavMode, reroute: Bool,
                       then: String? = nil, continuing: Bool = false) {
        let keepThen = reroute ? trip?.then : then  // a re-route keeps the stop on the way
        Task {
            let line = route.polyline.coordinates
            let plain = RouteGeometry.steps(from: route)
            let turns = plain.map { RouteGeometry.project(line, $0.point).segment + (RouteGeometry.project(line, $0.point).t > 0.5 ? 1 : 0) }
            // Both at once, each at most ~3 s: places at the turns ("to your left is a lovely cafe") and the weather.
            async let withPlaces = Self.placesAtTurns(plain, line: line, turns: turns)
            async let weather = reroute ? nil : NavWeather.fetch(at: line.first ?? target)
            let steps = await withPlaces
            begin(route: route, steps: steps, turns: turns, req: req, name: name, target: target, mode: mode, reroute: reroute,
                  then: keepThen, continuing: continuing, weather: await weather)
        }
    }

    /// A notable place within ~60 m of each turn and which side it's on, for the companion to mention now and
    /// then (the user's ask). At most 3 s: the route isn't held up by it.
    private nonisolated static func placesAtTurns(_ steps: [NavStepInfo], line: [CLLocationCoordinate2D], turns: [Int]) async -> [NavStepInfo] {
        var out = steps
        let found: [(Int, String)] = await withTaskGroup(of: [(Int, String)].self) { outer in
            outer.addTask {
                await withTaskGroup(of: (Int, String?).self) { group in
                    for i in steps.indices where !steps[i].text.isEmpty && steps[i].arrow != .arrive && i < 16 {
                        let k = turns[i]
                        let heading = k > 0 && k < line.count ? RouteGeometry.bearing(line[k - 1], line[k])
                            : k + 1 < line.count ? RouteGeometry.bearing(line[k], line[k + 1]) : 0
                        let point = steps[i].point
                        group.addTask { (i, await placeNear(point, heading: heading)) }
                    }
                    var list: [(Int, String)] = []
                    for await (i, poi) in group { if let poi { list.append((i, poi)) } }
                    return list
                }
            }
            outer.addTask {
                try? await Task.sleep(for: .seconds(3))
                return []
            }
            let first = await outer.next() ?? []
            outer.cancelAll()
            return first
        }
        for (i, poi) in found { out[i].poi = poi }
        return out
    }

    private nonisolated static func placeNear(_ point: CLLocationCoordinate2D, heading: Double) async -> String? {
        let request = MKLocalPointsOfInterestRequest(center: point, radius: 60)
        request.pointOfInterestFilter = MKPointOfInterestFilter(including: [.restaurant, .cafe, .bakery, .museum, .park,
                                                                            .theater, .nightlife, .landmark, .castle])
        guard let items = try? await MKLocalSearch(request: request).start().mapItems else { return nil }
        let here = CLLocation(latitude: point.latitude, longitude: point.longitude)
        guard let best = items.min(by: { $0.location.distance(from: here) < $1.location.distance(from: here) }),
              let name = best.name else { return nil }
        let kind = NearbySearch.name(best.pointOfInterestCategory).lowercased()
        let side = RouteGeometry.sideWords(heading: heading, toward: RouteGeometry.bearing(point, best.location.coordinate))
        return kind.isEmpty ? "\(name), \(side)" : "\(name), a \(kind), \(side)"
    }

    private func begin(route: MKRoute, steps: [NavStepInfo], turns: [Int], req: Int, name: String, target: CLLocationCoordinate2D,
                       mode: NavMode, reroute: Bool, then: String?, continuing: Bool, weather: String?) {
        guard !abandoned.contains(req) || reroute else { return log("request \(req) was given up on by Dotty") }
        let line = route.polyline.coordinates
        var t = Trip(route: nextRoute(), mode: mode, destination: name, target: target, line: line, turns: turns, steps: steps,
                     totalTime: route.expectedTravelTime, totalDistance: route.distance)
        t.step = RouteGeometry.firstSpoken(steps.map(\.text))
        t.then = then
        t.left = route.distance
        t.eta = Date().addingTimeInterval(route.expectedTravelTime)
        if trip != nil { t.lastReroute = Date() }  // replacing a route: the next re-route waits its 20 s
        trip = t
        log("\(reroute ? "re-route" : "route") \(t.route) to \(name): \(steps.count) steps, \(Int(route.distance)) m")
        var messages = NavMessages.route(req: reroute ? 0 : req, route: t.route, destination: name, mode: mode,
                                         distance: Int(route.distance), eta: localEpoch(t.eta), steps: steps, reroute: reroute)
        if continuing { messages[0]["continue"] = true }  // the next leg after a stop on the way
        if let weather { messages[0]["weather"] = weather }
        Task {
            await send(messages)
            if trip?.route == t.route { trip?.ready = true }
        }
        follow()
    }

    private func fail(_ req: Int, _ error: String) async {
        log("request \(req) failed: \(error)")
        await send([NavMessages.failure(req: req, route: nextRoute(), error: error, choices: [])])
    }

    /// Commands one after another (each waits for Dotty's reply: replies are matched by name).
    private func send(_ messages: [[String: Any]]) async {
        guard let link else { return }
        for var m in messages {
            guard let cmd = m.removeValue(forKey: "cmd") as? String else { continue }
            do {
                try await link.send(cmd, m, timeout: 5)
            } catch DottyError.refused(let why) {  // Dotty doesn't hold this route (gave up, or ended the trip)
                log("\(cmd) refused: \(why)")
                if cmd.hasPrefix("nav.") && trip?.route == m["route"] as? Int { finish() }
                return
            } catch {
                log("\(cmd) not sent: \(error.localizedDescription)")
                return
            }
        }
    }

    /// Dotty's clock is local: epoch seconds plus the UTC offset.
    private func localEpoch(_ d: Date) -> Int64 {
        Int64(d.timeIntervalSince1970) + Int64(TimeZone.current.secondsFromGMT(for: d))
    }

    private func street(_ item: MKMapItem) -> String {
        item.addressRepresentations?.fullAddress(includingRegion: false, singleLine: true)?
            .split(separator: ",").first.map(String.init) ?? ""
    }

    // MARK: - Following you

    /// Directions start from the pocket: "Always" is asked for when the Places screen opens (iOS first asks
    /// While Using, then offers Always).
    func askAlways() {
        switch manager.authorizationStatus {
        case .notDetermined: manager.requestWhenInUseAuthorization()
        case .authorizedWhenInUse: manager.requestAlwaysAuthorization()
        default: break
        }
    }

    var hasAlways: Bool { manager.authorizationStatus == .authorizedAlways }

    private func authorized() -> Bool {
        let s = manager.authorizationStatus
        if s == .notDetermined && UIApplication.shared.applicationState == .active { manager.requestWhenInUseAuthorization() }
        return s == .authorizedAlways || (s == .authorizedWhenInUse && UIApplication.shared.applicationState == .active)
    }

    private func follow() {
        guard let trip else { return }
        if simulate { simulateWalk() }  // still with location on: it keeps the app running in the background
        manager.desiredAccuracy = kCLLocationAccuracyBest
        manager.activityType = trip.mode == .driving ? .automotiveNavigation : trip.mode == .cycling ? .otherNavigation : .fitness
        manager.allowsBackgroundLocationUpdates = true
        manager.showsBackgroundLocationIndicator = true
        manager.startUpdatingLocation()
    }

    /// One fix (the last one if it's fresh), waiting up to 10 s.
    private func fix() async -> CLLocation? {
        if let here, Date().timeIntervalSince(here.timestamp) < 15 { return here }
        manager.desiredAccuracy = kCLLocationAccuracyBest
        manager.allowsBackgroundLocationUpdates = true
        manager.startUpdatingLocation()
        return await withCheckedContinuation { continuation in
            waitingForFix.append(continuation)
            Task { [weak self] in
                try? await Task.sleep(for: .seconds(10))
                self?.resolveFixes(nil)
            }
        }
    }

    private func resolveFixes(_ location: CLLocation?) {
        let waiting = waitingForFix
        waitingForFix.removeAll()
        for c in waiting { c.resume(returning: location) }
        if trip == nil && !waiting.isEmpty && location == nil { manager.stopUpdatingLocation() }
    }

    fileprivate func got(_ location: CLLocation) {
        guard location.horizontalAccuracy >= 0, location.horizontalAccuracy <= 50 || trip == nil else { return }  // a wild fix
        here = location
        resolveFixes(location)
        if trip == nil {
            if waitingForFix.isEmpty { manager.stopUpdatingLocation() }
            return
        }
        if !simulate { progress(at: location) }
    }

    private func progress(at location: CLLocation) {
        guard var t = trip, t.ready else { return }
        let p = location.coordinate
        // Off the route for a while: a new one from here (at most every 20 s).
        let off = RouteGeometry.distanceToRoute(t.line, p)
        if off > (t.mode == .driving ? 80 : t.mode == .cycling ? 50 : 30) {
            if t.offSince == nil { t.offSince = Date() }
        } else {
            t.offSince = nil
        }
        let leftAlong = RouteGeometry.metresAlong(t.line, from: p, to: t.line.count - 1)
        if let since = t.offSince, RouteGeometry.offRoute(distanceFromRoute: off, mode: t.mode, since: Date().timeIntervalSince(since)),
           Date().timeIntervalSince(t.lastReroute) > 20, RouteGeometry.rerouteAllowed(leftAlong: leftAlong) {
            t.lastReroute = Date()
            trip = t
            return reroute(from: p)
        }
        // Arrived?
        if RouteGeometry.arrived(toTarget: RouteGeometry.distance(p, t.target), leftAlong: leftAlong, mode: t.mode) {
            let route = t.route, next = t.then, mode = t.mode
            log("arrived at \(t.destination)" + (next.map { ", next \($0)" } ?? ""))
            finish()
            Task {
                var arrived: [String: Any] = ["cmd": "nav.arrived", "route": route]
                if let next { arrived["next"] = next }
                await send([arrived])
                if let next { await continueTo(next, mode: mode) }  // the next leg, on its own
            }
            return
        }
        // The next turn: the first step whose turn lies ahead of where you are on the line.
        let at = RouteGeometry.project(t.line, p).segment
        var next = t.steps.count - 1
        for i in t.step..<t.steps.count where t.turns[i] > at && !t.steps[i].text.isEmpty {
            next = i
            break
        }
        let toTurn = Int(RouteGeometry.metresAlong(t.line, from: p, to: t.turns[next]))
        t.left = leftAlong
        t.eta = Date().addingTimeInterval(t.totalDistance > 0 ? t.totalTime * t.left / t.totalDistance : 0)
        let changed = RouteGeometry.progressDue(stepChanged: next != t.step, metresChanged: toTurn - t.sentToTurn, mode: t.mode,
                                                sinceLast: Date().timeIntervalSince(t.sentAt))  // a keepalive every 15 s too
        t.step = next
        if changed {
            t.sentToTurn = toTurn
            t.sentAt = Date()
        }
        trip = t
        guard changed else { return }
        Task {
            await send([["cmd": "nav.progress", "route": t.route, "step": next, "toTurn": toTurn, "left": Int(t.left),
                         "eta": localEpoch(t.eta)]])
        }
    }

    private func reroute(from p: CLLocationCoordinate2D) {
        guard let t = trip else { return }
        log("off the route: a new one")
        Task {
            let target = MKMapItem(location: CLLocation(latitude: t.target.latitude, longitude: t.target.longitude), address: nil)
            guard let route = await directions(from: p, to: target, mode: t.mode), trip?.route == t.route else { return }  // still that trip
            start(route: route, req: 0, name: t.destination, target: t.target, mode: t.mode, reroute: true)
        }
    }

    /// The app's Stop (or Dotty gone): Dotty hears nav.ended. Dotty's own stop: nothing to tell.
    func stop(why: String?) {
        guard let t = trip else { return }
        log("trip stopped: \(why ?? "Dotty asked")")
        finish()
        if let why { Task { await send([["cmd": "nav.ended", "route": t.route, "why": why]]) } }
    }

    private func finish() {
        trip = nil
        simulation?.cancel()
        simulation = nil
        manager.stopUpdatingLocation()
        manager.allowsBackgroundLocationUpdates = false
    }

    /// Relay log › Simulate walk: fixes along the route at 1.4 m/s, one a second.
    private func simulateWalk() {
        simulation?.cancel()
        simulation = Task { [weak self] in
            guard let line = self?.trip?.line, line.count >= 2 else { return }
            var points: [CLLocationCoordinate2D] = []
            for i in 0..<(line.count - 1) {
                let d = RouteGeometry.distance(line[i], line[i + 1])
                let n = max(1, Int(d / 1.4))
                for k in 0..<n {
                    let f = Double(k) / Double(n)
                    points.append(CLLocationCoordinate2D(latitude: line[i].latitude + (line[i + 1].latitude - line[i].latitude) * f,
                                                         longitude: line[i].longitude + (line[i + 1].longitude - line[i].longitude) * f))
                }
            }
            points.append(line[line.count - 1])
            for p in points {
                try? await Task.sleep(for: .seconds(1))
                guard let self, self.trip != nil, !Task.isCancelled else { return }
                self.progress(at: CLLocation(latitude: p.latitude, longitude: p.longitude))
            }
        }
    }

    // MARK: - What's nearby

    private func nearby(req: Int, what: String, mode: NavMode) async {
        log("nearby \(req): \"\(what)\"")
        let background = UIApplication.shared.beginBackgroundTask(withName: "nav-nearby")
        defer { UIApplication.shared.endBackgroundTask(background) }
        guard authorized(), let from = await fix() else {
            return await send(NavMessages.places(req: req, ok: false, error: "the iPhone couldn't tell where it is", places: []))
        }
        let category = NearbySearch.category(what)
        var radius: Double = mode == .driving ? 5000 : 1000
        var items: [MKMapItem] = []
        for _ in 0..<2 where items.isEmpty {
            items = await search(what: what, category: category, around: from.coordinate, radius: radius)
            radius *= 2
        }
        let nearest = NearbySearch.nearest(items, distance: { $0.location.distance(from: from) })
        lastNearby = nearest
        let places = nearest.map { item in
            let metres = item.location.distance(from: from)
            return NavMessages.Place(name: item.name ?? what, category: NearbySearch.name(item.pointOfInterestCategory),
                                     street: street(item), direction: RouteGeometry.directionWords(from: from.coordinate, to: item.location.coordinate),
                                     distance: Int(metres), minutes: NearbySearch.minutes(metres: metres, mode: mode),
                                     phone: item.phoneNumber)
        }
        log("nearby \(req): \(places.count) found")
        await send(NavMessages.places(req: req, ok: true, error: nil, places: places))
    }

    private func search(what: String, category: MKPointOfInterestCategory?, around: CLLocationCoordinate2D, radius: Double) async -> [MKMapItem] {
        if let category {
            let request = MKLocalPointsOfInterestRequest(center: around, radius: radius)
            request.pointOfInterestFilter = MKPointOfInterestFilter(including: [category])
            return (try? await MKLocalSearch(request: request).start().mapItems) ?? []
        }
        let request = MKLocalSearch.Request()
        request.naturalLanguageQuery = what
        request.region = MKCoordinateRegion(center: around, latitudinalMeters: radius * 2, longitudinalMeters: radius * 2)
        let center = CLLocation(latitude: around.latitude, longitude: around.longitude)
        return ((try? await MKLocalSearch(request: request).start().mapItems) ?? []).filter { $0.location.distance(from: center) <= radius }
    }
}

// CoreLocation calls these on the main run loop's thread; nonisolated, they hop to the main actor (a
// MainActor closure called off main traps under Swift 6).
extension NavigationService: CLLocationManagerDelegate {
    nonisolated func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let last = locations.last else { return }
        Task { @MainActor in NavigationService.shared.got(last) }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        let text = error.localizedDescription
        Task { @MainActor in RelayService.shared.log("nav: location error: \(text)") }
    }
}
