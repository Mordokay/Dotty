import CoreLocation
import Foundation
import MapKit

/// How you're going (Dotty's names).
enum NavMode: String, Sendable {
    case walking, cycling, driving
}

/// Dotty's arrow names (nav_route.h).
enum NavArrow: String, Sendable {
    case straight, slightLeft = "slight-left", left, sharpLeft = "sharp-left", slightRight = "slight-right", right,
         sharpRight = "sharp-right", uturn, roundabout, arrive
}

/// One step as Dotty gets it, plus where its turn is.
struct NavStepInfo: Sendable {
    let text: String
    let street: String
    let distance: Int
    let arrow: NavArrow
    let exit: Int?
    let point: CLLocationCoordinate2D  // the turn: the start of this step's line
}

/// Directions maths, kept pure for the Swift checks (directions plan, Task 8): arrows from the route's
/// shape, metres to the next turn along the route, off-route and arrival, directions in words.
enum RouteGeometry {
    /// < 20° straight, 20-45 slight, 45-135 a turn, 135-170 sharp, > 170 a U-turn (+ = right); a roundabout
    /// (from Apple's words) with its exit.
    nonisolated static func arrow(turnDegrees: Double, instruction: String) -> (NavArrow, Int?) {
        let words = instruction.lowercased()
        if words.contains("roundabout") || words.contains(" exit") { return (.roundabout, exitNumber(words)) }
        if words.contains("u-turn") || words.contains("make a u") { return (.uturn, nil) }
        let a = abs(turnDegrees), right = turnDegrees > 0
        switch a {
        case ..<20: return (.straight, nil)
        case ..<45: return (right ? .slightRight : .slightLeft, nil)
        case ..<135: return (right ? .right : .left, nil)
        case ..<170: return (right ? .sharpRight : .sharpLeft, nil)
        default: return (.uturn, nil)
        }
    }

    nonisolated static func exitNumber(_ words: String) -> Int? {
        let ordinals = ["first": 1, "second": 2, "third": 3, "fourth": 4, "fifth": 5, "1st": 1, "2nd": 2, "3rd": 3, "4th": 4, "5th": 5]
        for (word, n) in ordinals where words.contains(word + " exit") { return n }
        return nil
    }

    /// "Turn right onto Rua Augusta" → "Rua Augusta"; "" when there's no street in it.
    nonisolated static func street(_ instruction: String) -> String {
        for marker in [" onto ", " on ", " toward ", " towards "] {
            if let r = instruction.range(of: marker) {
                return String(instruction[r.upperBound...]).trimmingCharacters(in: .whitespaces)
            }
        }
        return ""
    }

    nonisolated static func firstSpoken(_ texts: [String]) -> Int {
        texts.firstIndex { !$0.trimmingCharacters(in: .whitespaces).isEmpty } ?? 0
    }

    /// Compass bearing from a to b, degrees clockwise from north.
    nonisolated static func bearing(_ a: CLLocationCoordinate2D, _ b: CLLocationCoordinate2D) -> Double {
        let la1 = a.latitude * .pi / 180, la2 = b.latitude * .pi / 180, dl = (b.longitude - a.longitude) * .pi / 180
        let y = sin(dl) * cos(la2), x = cos(la1) * sin(la2) - sin(la1) * cos(la2) * cos(dl)
        return (atan2(y, x) * 180 / .pi + 360).truncatingRemainder(dividingBy: 360)
    }

    /// The turn at b coming from a, going to c: + right, − left (−180…180).
    nonisolated static func turnDegrees(_ a: CLLocationCoordinate2D, _ b: CLLocationCoordinate2D, _ c: CLLocationCoordinate2D) -> Double {
        var d = bearing(b, c) - bearing(a, b)
        while d > 180 { d -= 360 }
        while d < -180 { d += 360 }
        return d
    }

    nonisolated static func distance(_ a: CLLocationCoordinate2D, _ b: CLLocationCoordinate2D) -> Double {
        CLLocation(latitude: a.latitude, longitude: a.longitude).distance(from: CLLocation(latitude: b.latitude, longitude: b.longitude))
    }

    /// Where `p` falls on the line: the segment index and the fraction along it (a flat projection: fine at street scale).
    nonisolated static func project(_ line: [CLLocationCoordinate2D], _ p: CLLocationCoordinate2D) -> (segment: Int, t: Double, metres: Double) {
        var best = (segment: 0, t: 0.0, metres: Double.infinity)
        guard line.count >= 2 else { return (0, 0, line.first.map { distance($0, p) } ?? .infinity) }
        let k = cos(p.latitude * .pi / 180)
        for i in 0..<(line.count - 1) {
            let a = line[i], b = line[i + 1]
            let ax = a.longitude * k, ay = a.latitude, bx = b.longitude * k, by = b.latitude, px = p.longitude * k, py = p.latitude
            let dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy
            let t = len2 > 0 ? max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / len2)) : 0
            let q = CLLocationCoordinate2D(latitude: ay + t * dy, longitude: (ax + t * dx) / k)
            let m = distance(q, p)
            if m < best.metres { best = (i, t, m) }
        }
        return best
    }

    nonisolated static func distanceToRoute(_ line: [CLLocationCoordinate2D], _ p: CLLocationCoordinate2D) -> Double {
        project(line, p).metres
    }

    /// Metres along the line from where `p` falls on it to the point `index`.
    nonisolated static func metresAlong(_ line: [CLLocationCoordinate2D], from p: CLLocationCoordinate2D, to index: Int) -> Double {
        guard line.count >= 2, index > 0 else { return 0 }
        let at = project(line, p)
        if at.segment >= index { return 0 }
        var m = distance(line[at.segment], line[at.segment + 1]) * (1 - at.t)
        for i in (at.segment + 1)..<min(index, line.count - 1) { m += distance(line[i], line[i + 1]) }
        return m
    }

    nonisolated static func offRoute(distanceFromRoute: Double, mode: NavMode, since seconds: Double) -> Bool {
        let limit: Double = mode == .driving ? 80 : mode == .cycling ? 50 : 30
        return distanceFromRoute > limit && seconds >= 8
    }

    nonisolated static func arrived(distance: Double, mode: NavMode) -> Bool {
        distance <= (mode == .driving ? 40 : 25)
    }

    nonisolated static func directionWords(from a: CLLocationCoordinate2D, to b: CLLocationCoordinate2D) -> String {
        let names = ["north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"]
        return names[Int((bearing(a, b) + 22.5) / 45) % 8] + " of you"
    }

    /// An MKRoute's steps as Dotty gets them, with each turn's point and its arrow from the route's shape.
    static func steps(from route: MKRoute) -> [NavStepInfo] {
        let all = route.polyline.coordinates
        var out: [NavStepInfo] = []
        for (i, step) in route.steps.enumerated() {
            let coords = step.polyline.coordinates
            let point = coords.first ?? all.first ?? CLLocationCoordinate2D()
            let text = step.instructions
            var arrow: (NavArrow, Int?) = (.straight, nil)
            if i == route.steps.count - 1 && i > 0 {
                arrow = (.arrive, nil)
            } else if let previous = route.steps[safe: i - 1]?.polyline.coordinates, previous.count >= 2, coords.count >= 2 {
                arrow = Self.arrow(turnDegrees: turnDegrees(previous[previous.count - 2], point, coords[1]), instruction: text)
            } else {
                arrow = Self.arrow(turnDegrees: 0, instruction: text)
            }
            out.append(NavStepInfo(text: text, street: street(text), distance: Int(step.distance.rounded()), arrow: arrow.0,
                                   exit: arrow.1, point: point))
        }
        return out
    }
}

extension MKPolyline {
    var coordinates: [CLLocationCoordinate2D] {
        var out = [CLLocationCoordinate2D](repeating: CLLocationCoordinate2D(), count: pointCount)
        getCoordinates(&out, range: NSRange(location: 0, length: pointCount))
        return out
    }
}

extension Array {
    subscript(safe i: Int) -> Element? { indices.contains(i) ? self[i] : nil }
}
