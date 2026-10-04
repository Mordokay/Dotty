import Foundation
import simd

/// Several soft bodies stepped together so they can push each other. Each substep, after every
/// body has solved its own constraints, points that ended up inside another body are pushed back
/// out, the push shared between the point and the edge it hit. A small overlap is allowed so the
/// bodies' glass and light can meet and blend where they touch. A pushed body wakes up, dents,
/// slides away and springs back to its place.
final class JellyWorld {
    private(set) var bodies: [SoftBody]

    /// How deep one body may sink into another before being pushed out, in points.
    private let allowedOverlap: Float = 4

    private var clock: Double?
    private var accumulator = 0.0
    private var touching: Set<Int> = []
    /// Called when two bodies start touching, with how hard (0...1).
    var onContact: ((Float) -> Void)?

    init(bodies: [SoftBody]) {
        self.bodies = bodies
    }

    var isAsleep: Bool { bodies.allSatisfy(\.isAsleep) }

    /// The body under `location`, if any (with a little slack around the outline).
    func body(at location: CGPoint) -> Int? {
        let p = SIMD2(Float(location.x), Float(location.y))
        if let inside = bodies.firstIndex(where: { $0.contains(p) }) { return inside }
        return bodies.indices
            .map { ($0, simd_distance(bodies[$0].closestEdge(to: p).point, p)) }
            .filter { $0.1 < 10 }
            .min { $0.1 < $1.1 }?.0
    }

    func advance(to time: Double) {
        guard !isAsleep else {
            clock = time
            return
        }
        let last = clock ?? time
        clock = time
        accumulator += min(time - last, 0.1)
        let step = 1 / SoftBody.stepRate
        let h = Float(step) / Float(SoftBody.substepCount)
        var steps = 0
        while accumulator >= step, steps < 6 {
            let awake = bodies.filter { !$0.isAsleep }
            for body in awake { body.beginStep(Float(step), time: time) }
            for _ in 0..<SoftBody.substepCount {
                for body in awake { body.solve(h) }
                collide(time: time)
                for body in awake { body.finishSubstep(h) }
            }
            accumulator -= step
            steps += 1
        }
        for body in bodies where !body.isAsleep { body.settle(time) }
    }

    private func collide(time: Double) {
        var nowTouching: Set<Int> = []
        var hardest: Float = 0
        for a in bodies.indices {
            for b in bodies.indices where b > a {
                guard !(bodies[a].isAsleep && bodies[b].isAsleep) else { continue }
                let depth = max(separate(bodies[a], from: bodies[b]), separate(bodies[b], from: bodies[a]))
                if depth > 0 {
                    nowTouching.insert(a * 64 + b)
                    for body in [bodies[a], bodies[b]] where body.isAsleep {
                        body.wake(time)
                        body.setClock(time)
                    }
                    if !touching.contains(a * 64 + b) { hardest = max(hardest, min(1, depth / 12)) }
                }
            }
        }
        if hardest > 0 { onContact?(hardest) }
        touching = nowTouching
    }

    /// Pushes `a`'s points out of `b`. Returns the deepest penetration found (0 if none).
    private func separate(_ a: SoftBody, from b: SoftBody) -> Float {
        let boxA = a.bounds, boxB = b.bounds
        guard boxA.min.x < boxB.max.x, boxB.min.x < boxA.max.x, boxA.min.y < boxB.max.y, boxB.min.y < boxA.max.y else { return 0 }
        var deepest: Float = 0
        let n = b.points.count
        for i in a.points.indices {
            let p = a.points[i]
            guard p.x > boxB.min.x, p.x < boxB.max.x, p.y > boxB.min.y, p.y < boxB.max.y, b.contains(p) else { continue }
            let edge = b.closestEdge(to: p)
            let push = edge.point - p
            let depth = simd_length(push)
            deepest = max(deepest, depth)
            guard depth > allowedOverlap else { continue }
            let correction = push / depth * (depth - allowedOverlap) * 0.5
            a.points[i] += correction
            b.points[edge.index] -= correction * (1 - edge.t)
            b.points[(edge.index + 1) % n] -= correction * edge.t
        }
        return deepest
    }
}
