import Foundation
import simd

/// A position-based soft body: one closed ring of outline points held to a rest shape by shape
/// matching, edge lengths and area (pressure), with a springy stretch for the "boing". Pressing
/// dents and squeezes it, poking makes it wobble, dragging stretches it, and it always settles
/// back. Ported from the squishy-studio model (see Design/Research/squishy-soft-bodies.md),
/// anchored to its rest position so it behaves as a control rather than a free object.
///
/// Coordinates are points in the owning view, y down.
final class SoftBody {
    struct Shape {
        var size: CGSize
        var cornerRadius: CGFloat
    }

    private struct Dent {
        var point: SIMD2<Float>      // relative to the rest centre
        var radius: Float
        var depth: Float             // 0...1, eased toward `target`
        var target: Float
        var releasedAt: Double?
    }

    /// A drag, modelled like playdough: where the finger first touched decides how strongly each
    /// part of the body follows it (a smooth falloff over the rest shape), so pulling an edge
    /// stretches that edge out while the far side stays. Travel is rubber-banded, and the body
    /// keeps its area, so it thins as it stretches.
    private struct Grab {
        var start: SIMD2<Float>
        var weights: [Float]
        var limit: Float
        var displacement = SIMD2<Float>(0, 0)
        var released = false
    }

    // Tuning, from the original.
    private let edgeStiffness: Float = 26_000
    private let areaStiffness: Float = 5_200
    private let shapeStiffness: Float = 950
    private let anchorStiffness: Float = 300
    private let idleFirmness: Float = 3.2
    private var firmness: Float = 1
    private var lastRelease: Double?
    private let wobbleDecay: Float = 2.6
    private let drag: Float = 6.0  // the original's 0.12 suits free bodies; a control is anchored
    private let strainSpring: Float = 330
    private let strainDamping: Float = 7.5

    let center: SIMD2<Float>
    let shape: Shape
    private let rest: [SIMD2<Float>]
    private let restNormals: [SIMD2<Float>]
    private let scale: Float

    /// The outline. A JellyWorld also moves these to resolve collisions between bodies.
    var points: [SIMD2<Float>]
    private var velocities: [SIMD2<Float>]
    /// Stretch matrix offsets (xx, yy, xy) and their velocities: the jelly's "boing".
    private(set) var strain = SIMD3<Float>(0, 0, 0)
    private var strainVelocity = SIMD3<Float>(0, 0, 0)

    private var dents: [Dent] = []
    private var grab: Grab?
    private var pressStart: Double?
    private var pressPoint = SIMD2<Float>(0, 0)
    private(set) var squeeze: Float = 0
    /// How much light is being pushed into the body right now (0...1), for the inner glow.
    private(set) var energy: Float = 0

    // Per-step state, shared by the substeps.
    private var goal: [SIMD2<Float>] = []
    private var targetArea: Float = 0
    private var orientation: Float = 1
    private var previous: [SIMD2<Float>] = []

    private var clock: Double?
    private var accumulator = 0.0
    private var calmSince: Double?
    private(set) var isAsleep = true

    init(shape: Shape, center: CGPoint, count: Int = 40) {
        self.shape = shape
        self.center = SIMD2(Float(center.x), Float(center.y))
        scale = Float(min(shape.size.width, shape.size.height))
        let outline = Self.roundedRectOutline(size: shape.size, radius: shape.cornerRadius, count: count)
        rest = outline
        restNormals = Self.normals(of: outline)
        points = outline.map { $0 + SIMD2(Float(center.x), Float(center.y)) }
        velocities = Array(repeating: .zero, count: count)
    }

    // MARK: - Interaction

    func press(at location: CGPoint, time: Double) {
        let p = SIMD2(Float(location.x), Float(location.y))
        pressStart = time
        pressPoint = p
        dents.append(Dent(point: p - center, radius: scale * 0.9, depth: 0, target: 0.55, releasedAt: nil))
        wake(time)
    }

    func drag(to location: CGPoint, time: Double) {
        let p = SIMD2(Float(location.x), Float(location.y))
        if grab == nil, simd_distance(p, pressPoint) > 10 {
            startGrab(at: p)
        }
        if var current = grab, !current.released {
            let travel = p - current.start
            let distance = simd_length(travel)
            if distance > 1e-3 {
                let stretched = current.limit * (1 - exp(-distance / current.limit))
                current.displacement = travel / distance * stretched
            }
            grab = current
        }
        wake(time)
    }

    /// Ends a touch. Returns how strongly the body was squashed (0...1), for haptics.
    @discardableResult
    func release(at location: CGPoint, time: Double) -> Float {
        let held = time - (pressStart ?? time)
        let squashed = max(squeeze, 0.35)
        if grab != nil {
            // Let go of a stretch: it melts back (see update) instead of being flung.
            grab?.released = true
        } else if held < 0.19 {
            poke(at: pressPoint)
        } else {
            // Released after a squeeze: rebound past the rest shape.
            strainVelocity -= targetStrain() * 9
        }
        for i in dents.indices where dents[i].releasedAt == nil {
            dents[i].releasedAt = time
            dents[i].target = 0
        }
        pressStart = nil
        lastRelease = time
        squeeze = 0
        wake(time)
        return squashed
    }

    private func poke(at point: SIMD2<Float>) {
        let reach = scale * 0.9
        for i in points.indices {
            let d = simd_distance(points[i], point)
            guard d < reach else { continue }
            let falloff = 1 - d / reach
            velocities[i] -= restNormals[i] * 260 * falloff
        }
        let axis = simd_normalize(center - point + SIMD2(0.001, 0.001))
        kickSquash(axis: axis, strength: 2.8)
        energy = min(1, energy + 0.6)
    }

    private func startGrab(at finger: SIMD2<Float>) {
        dents.removeAll()
        let anchor = pressPoint - center
        let reach = 0.5 * Float(max(shape.size.width, shape.size.height))
        var weights = rest.map { exp(-simd_distance_squared($0, anchor) / (reach * reach)) }
        let strongest = weights.max() ?? 1
        weights = weights.map { $0 / max(strongest, 1e-4) }
        // The falloff stays smooth enough not to fold the outline as long as travel < ~reach.
        grab = Grab(start: pressPoint, weights: weights, limit: min(90, 0.85 * reach))
    }

    private func kickSquash(axis a: SIMD2<Float>, strength s: Float) {
        let bulge = s * 0.73
        strainVelocity.x += -s * a.x * a.x + bulge * a.y * a.y
        strainVelocity.y += -s * a.y * a.y + bulge * a.x * a.x
        strainVelocity.z += -s * a.x * a.y - bulge * a.x * a.y
    }

    func wake(_ time: Double) {
        if isAsleep { clock = time }
        isAsleep = false
        calmSince = nil
    }

    // MARK: - Simulation

    static let stepRate = 120.0
    static let substepCount = 4

    /// Advances a body on its own to `time` in fixed 1/120 s steps. Bodies that share a
    /// JellyWorld are stepped by the world instead, so they can collide.
    func advance(to time: Double) {
        guard !isAsleep else { return }
        let last = clock ?? time
        clock = time
        accumulator += min(time - last, 0.1)
        let step = 1 / Self.stepRate
        var steps = 0
        while accumulator >= step, steps < 6 {
            beginStep(Float(step), time: time)
            let h = Float(step) / Float(Self.substepCount)
            for _ in 0..<Self.substepCount {
                solve(h)
                finishSubstep(h)
            }
            accumulator -= step
            steps += 1
        }
        settle(time)
    }

    /// The time this body last advanced to, so a world can keep it in step.
    func setClock(_ time: Double) { clock = time }

    private func targetStrain() -> SIMD3<Float> {
        guard squeeze > 0 else { return .zero }
        let a = simd_normalize(center - pressPoint + SIMD2(0.001, 0.001))
        let compress = 0.18 * squeeze
        let bulge = 0.13 * squeeze
        return SIMD3(-compress * a.x * a.x + bulge * a.y * a.y,
                     -compress * a.y * a.y + bulge * a.x * a.x,
                     -(compress + bulge) * a.x * a.y)
    }

    /// Prepares one fixed step: squeeze, glow, stretch spring, dents and the goal shape.
    func beginStep(_ dt: Float, time: Double) {
        // Squeeze builds while a press is held still.
        if let start = pressStart, grab == nil {
            let held = Float(time - start)
            let t = min(max((held - 0.15) / 0.75, 0), 1)
            squeeze = 1 - pow(1 - t, 3)
        }
        energy += ((pressStart != nil ? 0.45 + 0.55 * squeeze : 0) - energy) * min(1, dt * (pressStart != nil ? 10 : 2.5))

        // The springy stretch.
        let target = targetStrain()
        strainVelocity += (strainSpring * (target - strain) - strainDamping * strainVelocity) * dt
        strain += strainVelocity * dt
        strain = simd_clamp(strain, SIMD3(-0.42, -0.42, -0.3), SIMD3(0.5, 0.5, 0.3))

        // Dents ease in while pressed and recover once released.
        for i in dents.indices {
            let rate: Float = dents[i].releasedAt == nil ? 14 : 5
            dents[i].depth += (dents[i].target - dents[i].depth) * min(1, dt * rate)
        }
        dents.removeAll { $0.releasedAt != nil && $0.depth < 0.01 }

        // Goal shape: the dented rest outline, stretched, around the rest centre.
        goal = rest
        for i in goal.indices {
            var push: Float = 0
            for dent in dents {
                let d2 = simd_distance_squared(rest[i], dent.point)
                let r2 = dent.radius * dent.radius
                if d2 < r2 {
                    let f = 1 - d2 / r2
                    push += 0.2 * scale * dent.depth * f * f
                }
            }
            goal[i] -= restNormals[i] * push
        }
        let restArea = Self.signedArea(goal)
        // The shape is matched around where the body is now, so a neighbour can push it aside
        // (a gentle anchor in `solve` brings it home). A body being dragged stays pinned to its
        // place instead, so pulling it stretches it.
        let held = grab.map { !$0.released } ?? false
        let goalCenter = held ? center : centroid
        // Under a finger the body is soft; left alone it is firmer, so pushes move it more than they dent it.
        let recentlyTouched = lastRelease.map { time - $0 < 1.0 } ?? false
        firmness = pressStart != nil || held || recentlyTouched ? 1 : idleFirmness
        for i in goal.indices {
            let p = goal[i]
            goal[i] = goalCenter + SIMD2(p.x * (1 + strain.x) + p.y * strain.z, p.x * strain.z + p.y * (1 + strain.y))
        }
        let determinant = (1 + strain.x) * (1 + strain.y) - strain.z * strain.z
        targetArea = restArea * determinant * (1 - 0.1 * squeeze)
        orientation = restArea >= 0 ? 1 : -1

        // A drag moves the goal itself, weighted by the grab's falloff. Once released, the
        // stretch melts back quickly so the body returns without a big rebound.
        if var current = grab {
            if current.released {
                current.displacement *= exp(-11 * dt)
                grab = simd_length(current.displacement) < 0.2 ? nil : current
            } else {
                grab = current
            }
            for i in goal.indices { goal[i] += current.weights[i] * current.displacement }
        }
    }

    /// One substep: predict positions, then satisfy edge, area, shape and grab constraints.
    func solve(_ h: Float) {
        let n = points.count
        previous = points
        do {
            for i in 0..<n { points[i] += velocities[i] * h }

            // Edge lengths toward the goal's.
            let edgeCompliance = 1 / (edgeStiffness * h * h)
            for i in 0..<n {
                let j = (i + 1) % n
                let length = simd_distance(goal[i], goal[j])
                let d = points[j] - points[i]
                let current = simd_length(d)
                guard current > 1e-4 else { continue }
                let delta = d / current * ((current - length) / (2 + edgeCompliance))
                points[i] += delta
                points[j] -= delta
            }

            // Area (pressure), stiffening when crushed.
            let area = Self.signedArea(points)
            var areaCompliance = 1 / (areaStiffness * h * h)
            if abs(area) < abs(targetArea) * 0.88 { areaCompliance /= 16 }
            var gradients = [SIMD2<Float>](repeating: .zero, count: n)
            var sum: Float = 0
            for i in 0..<n {
                let next = points[(i + 1) % n], prev = points[(i + n - 1) % n]
                gradients[i] = 0.5 * SIMD2(next.y - prev.y, prev.x - next.x)
                sum += simd_length_squared(gradients[i])
            }
            let lambda = -(area - targetArea) / (sum + areaCompliance)
            for i in 0..<n { points[i] += gradients[i] * lambda }

            // Pull toward the goal shape.
            let pull = 1 / (1 + 1 / (shapeStiffness * firmness * h * h))
            for i in 0..<n { points[i] += (goal[i] - points[i]) * pull }

            // A gentle anchor brings a pushed body back to its place.
            let anchor = 1 / (1 + 1 / (anchorStiffness * h * h))
            let home = (center - centroid) * anchor
            for i in 0..<n { points[i] += home }

            // Folds: while stretched, a point that turns the wrong way (the rest shapes are convex)
            // is pulled firmly back and smoothed against its neighbours, so the outline never
            // crosses itself.
            if grab != nil {
                let firm = 1 / (1 + 1 / (9_000 * h * h))
                for i in 0..<n {
                    let prev = points[(i + n - 1) % n], next = points[(i + 1) % n]
                    let a = points[i] - prev, b = next - points[i]
                    if (a.x * b.y - a.y * b.x) * orientation < 0 {
                        points[i] += (goal[i] - points[i]) * firm
                        points[i] += ((prev + next) * 0.5 - points[i]) * 0.3
                    }
                }
            }

            // While held, the grabbed part follows the finger closely.
            if let current = grab, !current.released {
                for i in 0..<n {
                    let stiffness = 30_000 * current.weights[i] * current.weights[i]
                    guard stiffness > 1 else { continue }
                    let f = 1 / (1 + 1 / (stiffness * h * h))
                    points[i] += (goal[i] - points[i]) * f
                }
            }

        }
    }

    /// Ends a substep: new velocities from the moves (after any collisions), where whole-body
    /// motion keeps going and wobble decays.
    func finishSubstep(_ h: Float) {
        let n = points.count
        var mean = SIMD2<Float>(0, 0)
        for i in 0..<n {
            velocities[i] = (points[i] - previous[i]) / h
            mean += velocities[i]
        }
        mean /= Float(n)
        // Buttons never turn: take out any spin, keeping their slide and their wobble.
        let centre = centroid
        var angularMomentum: Float = 0
        var inertia: Float = 0
        for i in 0..<n {
            let r = points[i] - centre
            let v = velocities[i] - mean
            angularMomentum += r.x * v.y - r.y * v.x
            inertia += simd_length_squared(r)
        }
        let spin = angularMomentum / max(inertia, 1e-3)
        for i in 0..<n {
            let r = points[i] - centre
            velocities[i] -= SIMD2(-spin * r.y, spin * r.x)
        }
        let keepWobble = exp(-wobbleDecay * h)
        let keepMotion = exp(-drag * h)
        for i in 0..<n {
            velocities[i] = mean * keepMotion + (velocities[i] - mean) * keepWobble
        }
    }

    /// Puts the body to sleep once it has been calm for half a second.
    func settle(_ time: Double) {
        let fastest = velocities.reduce(Float(0)) { max($0, simd_length($1)) }
        let calm = fastest < 2 && simd_length(strainVelocity) < 0.02 && simd_length(strain) < 0.003
            && dents.isEmpty && grab == nil && pressStart == nil && energy < 0.01
        if calm {
            if calmSince == nil { calmSince = time }
            if let since = calmSince, time - since > 0.5 {
                points = rest.map { $0 + center }
                velocities = velocities.map { _ in .zero }
                strain = .zero
                strainVelocity = .zero
                energy = 0
                isAsleep = true
            }
        } else {
            calmSince = nil
        }
    }

    // MARK: - Geometry

    /// The outline's bounding box.
    var bounds: (min: SIMD2<Float>, max: SIMD2<Float>) {
        var low = SIMD2<Float>(repeating: .infinity)
        var high = SIMD2<Float>(repeating: -.infinity)
        for p in points {
            low = simd_min(low, p)
            high = simd_max(high, p)
        }
        return (low, high)
    }

    /// Whether `p` is inside the outline (even-odd ray cast).
    func contains(_ p: SIMD2<Float>) -> Bool {
        var inside = false
        var j = points.count - 1
        for i in points.indices {
            let a = points[i], b = points[j]
            if (a.y > p.y) != (b.y > p.y), p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x {
                inside.toggle()
            }
            j = i
        }
        return inside
    }

    /// The closest point on the outline to `p`: the edge's first index, how far along it (0...1),
    /// and the point itself.
    func closestEdge(to p: SIMD2<Float>) -> (index: Int, t: Float, point: SIMD2<Float>) {
        var best = (index: 0, t: Float(0), point: points[0])
        var bestDistance = Float.infinity
        for i in points.indices {
            let a = points[i], b = points[(i + 1) % points.count]
            let ab = b - a
            let t = min(max(simd_dot(p - a, ab) / max(simd_length_squared(ab), 1e-6), 0), 1)
            let q = a + ab * t
            let d = simd_distance_squared(p, q)
            if d < bestDistance {
                bestDistance = d
                best = (i, t, q)
            }
        }
        return best
    }

    /// The affine transform that best maps the rest outline onto the current one (least
    /// squares): its 2×2 part (a, b, c, d as in CGAffineTransform) captures stretch, shear and
    /// turn, and the centroid gives the move. Labels use it to stay inside the deformed body.
    var deformation: (a: Float, b: Float, c: Float, d: Float) {
        let centre = centroid
        var srr = SIMD3<Float>(0, 0, 0)          // Σ rx², Σ rx·ry, Σ ry²
        var spr = SIMD4<Float>(0, 0, 0, 0)       // Σ px·rx, Σ px·ry, Σ py·rx, Σ py·ry
        for (r, q) in zip(rest, points) {
            let p = q - centre
            srr += SIMD3(r.x * r.x, r.x * r.y, r.y * r.y)
            spr += SIMD4(p.x * r.x, p.x * r.y, p.y * r.x, p.y * r.y)
        }
        let det = srr.x * srr.z - srr.y * srr.y
        guard abs(det) > 1e-3 else { return (1, 0, 0, 1) }
        let i00 = srr.z / det, i01 = -srr.y / det, i11 = srr.x / det
        // A = (Σ p rᵀ)(Σ r rᵀ)⁻¹, then kept within sane stretch so text stays legible.
        var m00 = spr.x * i00 + spr.y * i01, m01 = spr.x * i01 + spr.y * i11
        var m10 = spr.z * i00 + spr.w * i01, m11 = spr.z * i01 + spr.w * i11
        let scaleX = (m00 * m00 + m10 * m10).squareRoot(), scaleY = (m01 * m01 + m11 * m11).squareRoot()
        let limitX = min(max(scaleX, 0.6), 1.6) / max(scaleX, 1e-4), limitY = min(max(scaleY, 0.6), 1.6) / max(scaleY, 1e-4)
        m00 *= limitX; m10 *= limitX; m01 *= limitY; m11 *= limitY
        return (m00, m10, m01, m11)
    }

    /// The centroid of the current outline.
    var centroid: SIMD2<Float> { points.reduce(.zero, +) / Float(points.count) }

    static func signedArea(_ p: [SIMD2<Float>]) -> Float {
        var sum: Float = 0
        for i in p.indices {
            let a = p[i], b = p[(i + 1) % p.count]
            sum += a.x * b.y - b.x * a.y
        }
        return sum / 2
    }

    /// Outward normals of a closed outline (clockwise on screen, y down).
    private static func normals(of p: [SIMD2<Float>]) -> [SIMD2<Float>] {
        let sign: Float = signedArea(p) > 0 ? 1 : -1
        return p.indices.map { i in
            let t = p[(i + 1) % p.count] - p[(i + p.count - 1) % p.count]
            return simd_normalize(SIMD2(t.y, -t.x)) * sign
        }
    }

    /// Points spaced evenly along a rounded rectangle centred on the origin, clockwise from the top.
    private static func roundedRectOutline(size: CGSize, radius: CGFloat, count: Int) -> [SIMD2<Float>] {
        let w = Float(size.width), h = Float(size.height)
        let r = Float(min(radius, min(size.width, size.height) / 2))
        let straightX = w - 2 * r, straightY = h - 2 * r
        let arc = Float.pi / 2 * r
        let perimeter = 2 * straightX + 2 * straightY + 4 * arc
        var points: [SIMD2<Float>] = []
        for k in 0..<count {
            var s = Float(k) / Float(count) * perimeter
            // Start at the top centre, going clockwise (right along the top).
            s += straightX / 2 + 0
            s = s.truncatingRemainder(dividingBy: perimeter)
            let segments: [(Float, (Float) -> SIMD2<Float>)] = [
                (straightX, { t in SIMD2(-w / 2 + r + t, -h / 2) }),
                (arc, { t in let a = -Float.pi / 2 + t / r; return SIMD2(w / 2 - r + r * cos(a), -h / 2 + r + r * sin(a)) }),
                (straightY, { t in SIMD2(w / 2, -h / 2 + r + t) }),
                (arc, { t in let a = t / r; return SIMD2(w / 2 - r + r * cos(a), h / 2 - r + r * sin(a)) }),
                (straightX, { t in SIMD2(w / 2 - r - t, h / 2) }),
                (arc, { t in let a = Float.pi / 2 + t / r; return SIMD2(-w / 2 + r + r * cos(a), h / 2 - r + r * sin(a)) }),
                (straightY, { t in SIMD2(-w / 2, h / 2 - r - t) }),
                (arc, { t in let a = Float.pi + t / r; return SIMD2(-w / 2 + r + r * cos(a), -h / 2 + r + r * sin(a)) }),
            ]
            for (length, point) in segments {
                if s <= length || length == segments.last?.0 && s <= length + 0.001 {
                    points.append(point(s))
                    break
                }
                s -= length
            }
        }
        return points
    }
}
