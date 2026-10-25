import Foundation

/// The directions messages for Dotty (the plan's "Messages" table): a route as a nav.route header and a
/// nav.step per step, nearby places as nav.places + a nav.place each — every command ≤ 512 bytes.
enum NavMessages {
    nonisolated static let maxBytes = 512

    nonisolated static func route(req: Int, route: Int, destination: String, mode: NavMode, distance: Int, eta: Int64,
                                  steps: [NavStepInfo], reroute: Bool) -> [[String: Any]] {
        var head: [String: Any] = ["cmd": "nav.route", "req": req, "route": route, "ok": true,
                                   "destination": cut(destination, 60), "mode": mode.rawValue, "distance": distance,
                                   "eta": eta, "count": steps.count]
        if reroute { head["reroute"] = true }
        var out = [head]
        for (i, s) in steps.enumerated() {
            var step: [String: Any] = ["cmd": "nav.step", "route": route, "i": i, "street": cut(s.street, 80),
                                       "distance": s.distance, "arrow": s.arrow.rawValue]
            if let exit = s.exit { step["exit"] = exit }
            if let poi = s.poi, !poi.isEmpty { step["poi"] = cut(poi, 60) }
            step["text"] = fit(s.text, into: step)
            out.append(step)
        }
        return out
    }

    nonisolated static func failure(req: Int, route: Int, error: String?, choices: [(name: String, distance: Int)]) -> [String: Any] {
        var msg: [String: Any] = ["cmd": "nav.route", "req": req, "route": route, "ok": false]
        if let error { msg["error"] = cut(error, 200) }
        if !choices.isEmpty {
            msg["choices"] = choices.prefix(3).enumerated().map { ["n": $0.offset + 1, "name": cut($0.element.name, 60), "distance": $0.element.distance] }
        }
        return msg
    }

    struct Place {
        let name, category, street, direction: String
        let distance, minutes: Int
        let phone: String?
    }

    nonisolated static func places(req: Int, ok: Bool, error: String?, places: [Place]) -> [[String: Any]] {
        var head: [String: Any] = ["cmd": "nav.places", "req": req, "ok": ok, "count": ok ? places.count : 0]
        if let error { head["error"] = cut(error, 200) }
        var out = [head]
        for (i, p) in places.enumerated() {
            var m: [String: Any] = ["cmd": "nav.place", "req": req, "n": i + 1, "name": cut(p.name, 80),
                                    "category": cut(p.category, 40), "street": cut(p.street, 80), "distance": p.distance,
                                    "minutes": p.minutes, "direction": p.direction]
            if let phone = p.phone { m["phone"] = cut(phone, 30) }
            out.append(m)
        }
        return out
    }

    /// The longest start of `text` (whole characters) that keeps the command within maxBytes.
    nonisolated static func fit(_ text: String, into message: [String: Any]) -> String {
        var t = text
        while !t.isEmpty {
            var m = message
            m["text"] = t
            if let data = try? JSONSerialization.data(withJSONObject: m), data.count <= maxBytes { return t }
            t = String(t.dropLast(max(1, t.count / 10)))
        }
        return t
    }

    nonisolated static func cut(_ s: String, _ max: Int) -> String { s.count <= max ? s : String(s.prefix(max)) }
}
