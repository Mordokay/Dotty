import SwiftUI
import UIKit

/// The base of every screen: soft coloured lights glowing in the void, with fireflies flashing
/// and drifting through them. Nothing here simulates light: the lights are radial gradients
/// that Core Animation drifts and breathes on its own, and each firefly is a small gradient
/// layer looping forever along its own path, so neither our code nor SwiftUI does any work per
/// frame. A few nearer fireflies drift in
/// front of the content. Tilting the phone shifts each layer by its depth (parallax), and the
/// lights also lag behind the content as it scrolls.
struct LightField<Content: View>: View {
    var lights: [Color] = [DottyLight.lagoon.color, DottyLight.bloom.color, DottyLight.firefly.color]
    /// How many fireflies live behind the content.
    var motes = 32
    /// How many drift in front of it.
    var nearMotes = 12
    var seed: UInt64 = 5
    /// The content's scroll offset, read on the next frame (scrolling never re-renders views).
    var scroll: ScrollProbe?
    @ViewBuilder var content: Content

    /// The out-of-focus fireflies sit closer to the lens than anything else (even `Depth.near`),
    /// so they shift the most when tilting.
    private static var bokehDepth: CGFloat { 1.45 }

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.renderQuality) private var quality

    var body: some View {
        let low = quality.isLow
        ZStack {
            SkyLayer(configuration: .init(lights: lights, fireflies: low ? motes / 2 : motes, seed: seed,
                                          still: reduceMotion, depth: Depth.back, showsLights: true),
                     scroll: scroll)
                .ignoresSafeArea()
                .accessibilityHidden(true)

            content

            if nearMotes > 0 && !low {
                SkyLayer(configuration: .init(lights: lights, fireflies: nearMotes, seed: seed &+ 7,
                                              still: reduceMotion, depth: Self.bokehDepth, showsLights: false),
                         scroll: nil)
                    .ignoresSafeArea()
                    .allowsHitTesting(false)
                    .accessibilityHidden(true)
            }
        }
        .onAppear { MotionTilt.shared.start() }
        .onDisappear { MotionTilt.shared.stop() }
    }
}

/// Holds a scroll offset for a LightField. Writing to it doesn't re-render any view: the field
/// reads it on its next frame. Keep one in @State and set it from onScrollGeometryChange.
final class ScrollProbe {
    var offset: CGFloat = 0
}

/// One layer of the sky (the lights and fireflies behind the content, or the near fireflies).
private struct SkyLayer: UIViewRepresentable {
    struct Configuration: Equatable {
        var lights: [Color]
        var fireflies: Int
        var seed: UInt64
        var still: Bool
        var depth: CGFloat
        var showsLights: Bool
    }

    var configuration: Configuration
    var scroll: ScrollProbe?

    func makeUIView(context: Context) -> SkyView {
        let view = SkyView()
        view.configuration = configuration
        view.scroll = scroll
        return view
    }

    func updateUIView(_ view: SkyView, context: Context) {
        view.scroll = scroll
        if view.configuration != configuration {
            view.configuration = configuration
        }
    }
}

/// The sky as Core Animation layers: gradient lights with drift and breathing animations, and
/// fireflies that each loop forever along their own path. A display link at 30 fps only moves the layers for
/// parallax.
private final class SkyView: UIView {
    var configuration: SkyLayer.Configuration? {
        didSet { rebuild() }
    }
    var scroll: ScrollProbe?

    private let lightsLayer = CALayer()
    private let firefliesLayer = CALayer()
    private var link: CADisplayLink?
    private var builtSize: CGSize = .zero

    override init(frame: CGRect) {
        super.init(frame: frame)
        isUserInteractionEnabled = false
        layer.addSublayer(lightsLayer)
        layer.addSublayer(firefliesLayer)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func layoutSubviews() {
        super.layoutSubviews()
        if bounds.size != builtSize { rebuild() }
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil, link == nil {
            let link = CADisplayLink(target: self, selector: #selector(followTiltAndScroll))
            link.preferredFrameRateRange = CAFrameRateRange(minimum: 20, maximum: 30, preferred: 30)
            link.add(to: .main, forMode: .common)
            self.link = link
        } else if window == nil {
            link?.invalidate()
            link = nil
        }
    }

    /// Parallax: the only per-frame work, moving two layers.
    @objc private func followTiltAndScroll() {
        guard let configuration else { return }
        let tilt = MotionTilt.shared.sample()
        let far = MotionTilt.shift(tilt, depth: Depth.far)
        let fireflies = MotionTilt.shift(tilt, depth: configuration.depth)
        let scrolled = (scroll?.offset ?? 0) * Depth.far
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        lightsLayer.setAffineTransform(CGAffineTransform(translationX: -far.width, y: -far.height - scrolled))
        firefliesLayer.setAffineTransform(CGAffineTransform(translationX: -fireflies.width, y: -fireflies.height))
        CATransaction.commit()
    }

    private func rebuild() {
        guard let configuration, bounds.width > 0 else { return }
        builtSize = bounds.size
        backgroundColor = configuration.showsLights ? UIColor(Color.void) : .clear
        lightsLayer.frame = bounds
        firefliesLayer.frame = bounds
        buildLights(configuration)
        buildFireflies(configuration)
    }

    // MARK: Lights

    /// Where the lights sit, spread over the screen so they meet at their edges.
    private static let spots: [CGPoint] = [CGPoint(x: 0.18, y: 0.2), CGPoint(x: 0.86, y: 0.42), CGPoint(x: 0.3, y: 0.82), CGPoint(x: 0.8, y: 0.95)]

    private func buildLights(_ configuration: SkyLayer.Configuration) {
        lightsLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        guard configuration.showsLights else { return }
        var generator = SeededGenerator(seed: configuration.seed)
        let size = bounds.size
        for (index, color) in configuration.lights.enumerated() {
            let spot = Self.spots[index % Self.spots.count]
            let radius = size.width * CGFloat.random(in: 0.55...0.8, using: &generator)
            let light = CAGradientLayer()
            light.type = .radial
            light.startPoint = CGPoint(x: 0.5, y: 0.5)
            light.endPoint = CGPoint(x: 1, y: 1)
            let ui = UIColor(color)
            light.colors = [ui.withAlphaComponent(0.34).cgColor, ui.withAlphaComponent(0.13).cgColor, ui.withAlphaComponent(0).cgColor]
            light.locations = [0, 0.34, 1]
            light.bounds = CGRect(x: 0, y: 0, width: radius * 2, height: radius * 2)
            light.position = CGPoint(x: spot.x * size.width, y: spot.y * size.height)
            lightsLayer.addSublayer(light)

            guard !configuration.still else { continue }
            // Drift and breathe, out of step with each other. Core Animation runs these itself.
            let period = Durations.drift + Double(index) * 3.5
            let phase = Double.random(in: 0...period, using: &generator)
            let drift = CABasicAnimation(keyPath: "position")
            drift.toValue = CGPoint(x: light.position.x + size.width * 0.1 * (index.isMultiple(of: 2) ? 1 : -1),
                                    y: light.position.y + size.height * 0.06)
            drift.duration = period / 2
            drift.autoreverses = true
            drift.repeatCount = .infinity
            drift.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
            drift.timeOffset = phase
            drift.isRemovedOnCompletion = false
            light.add(drift, forKey: "drift")
            let breathe = CABasicAnimation(keyPath: "transform.scale")
            breathe.fromValue = 0.94
            breathe.toValue = 1.08
            breathe.duration = period / 3
            breathe.autoreverses = true
            breathe.repeatCount = .infinity
            breathe.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
            breathe.timeOffset = phase / 2
            breathe.isRemovedOnCompletion = false
            light.add(breathe, forKey: "breathe")
        }
    }

    // MARK: Fireflies

    /// The bracelet colours pushed to full saturation, so the fireflies glow rich.
    private static let palette: [UIColor] = DottyLight.allCases.map { light in
        var hue: CGFloat = 0, saturation: CGFloat = 0, brightness: CGFloat = 0, alpha: CGFloat = 0
        UIColor(light.color).getHue(&hue, saturation: &saturation, brightness: &brightness, alpha: &alpha)
        return UIColor(hue: hue, saturation: min(1, saturation * 1.2 + 0.12), brightness: 1, alpha: 1)
    }

    /// Fireflies never appear or vanish: each one lives as long as the sky, wandering a closed
    /// loop of curves (with a small orbit on top, so its path curls), dimming to 20% and
    /// brightening again, growing and shrinking, and drifting through colours. Every loop has
    /// its own length, so the sky never visibly repeats.
    private func buildFireflies(_ configuration: SkyLayer.Configuration) {
        firefliesLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        var generator = SeededGenerator(seed: configuration.seed &* 31)
        let near = configuration.depth > 1
        let still = configuration.still
        let count = configuration.fireflies
        // Spread their homes over a grid of cells, so they cover the screen without clumping.
        let columns = max(1, Int((Double(count) * Double(bounds.width / bounds.height)).squareRoot().rounded()))
        let rows = max(1, Int((Double(count) / Double(columns)).rounded(.up)))
        let area = bounds.insetBy(dx: 12, dy: 12)
        let reach = min(bounds.width, bounds.height) * (near ? 0.42 : 0.3)

        for index in 0..<count {
            func random(_ range: ClosedRange<CGFloat>) -> CGFloat { CGFloat.random(in: range, using: &generator) }
            func randomTime(_ range: ClosedRange<Double>) -> Double { Double.random(in: range, using: &generator) }

            let cell = CGRect(x: area.minX + area.width * CGFloat(index % columns) / CGFloat(columns),
                              y: area.minY + area.height * CGFloat(index / columns) / CGFloat(rows),
                              width: area.width / CGFloat(columns), height: area.height / CGFloat(rows))
            let home = CGPoint(x: random(cell.minX...cell.maxX), y: random(cell.minY...cell.maxY))

            // The firefly: a container that wanders, holding a glow that orbits within it.
            let wanderer = CALayer()
            wanderer.position = home
            // Near ones are out of focus, like bokeh close to the lens: big, soft and dim, so they
            // read as in front of everything, never as part of a control.
            let size = near ? random(54...76) : random(15...22)
            let glow = CAGradientLayer()
            glow.type = .radial
            glow.startPoint = CGPoint(x: 0.5, y: 0.5)
            glow.endPoint = CGPoint(x: 1, y: 1)
            glow.locations = near ? [0, 0.5, 0.78, 1] : [0, 0.14, 0.38, 1]
            glow.bounds = CGRect(x: 0, y: 0, width: size, height: size)
            glow.position = .zero
            wanderer.addSublayer(glow)
            firefliesLayer.addSublayer(wanderer)

            // Its colours: three neighbouring ones from the palette, cycled (opposite colours
            // would fade through grey).
            var colours: [UIColor] = []
            var pick = Int.random(in: 0..<Self.palette.count, using: &generator)
            for _ in 0..<3 {
                colours.append(Self.palette[pick])
                pick = (pick + Int.random(in: 1...2, using: &generator)) % Self.palette.count
            }
            let stops = colours.map(near ? Self.bokehColors : Self.glowColors)
            glow.colors = stops[0]
            glow.add(Self.loop("colors", stops, duration: randomTime(14...28), phase: randomTime(0...28)), forKey: "colour")

            // Flashing: bright, then dim to about 20%, and back, at an uneven rhythm.
            let flashes = 2 * Int.random(in: 2...4, using: &generator)
            let brightness = (0..<flashes).map { $0.isMultiple(of: 2) ? Double(random(0.9...1)) : Double(random(0.2...0.32)) }
            wanderer.opacity = Float(brightness[0])
            wanderer.add(Self.loop("opacity", brightness, duration: randomTime(5...11), phase: randomTime(0...11)), forKey: "flash")

            guard !still else { continue }

            // Growing and shrinking.
            let scales = (0..<Int.random(in: 4...6, using: &generator)).map { _ in Double(random(0.7...1.3)) }
            glow.add(Self.loop("transform.scale", scales, duration: randomTime(4...9), phase: randomTime(0...9)), forKey: "size")

            // Wandering: a closed loop of curves through random waypoints around its home.
            let waypoints = (0..<Int.random(in: 6...9, using: &generator)).map { _ in
                let angle = random(0...(2 * .pi)), distance = random(reach * 0.25...reach)
                return CGPoint(x: min(max(home.x + cos(angle) * distance, bounds.minX), bounds.maxX),
                               y: min(max(home.y + sin(angle) * distance, bounds.minY), bounds.maxY))
            }
            let path = Self.closedCurve(through: waypoints)
            let speed = near ? random(16...30) : random(9...20)
            let wander = CAKeyframeAnimation(keyPath: "position")
            wander.path = path
            wander.duration = Double(Self.length(of: waypoints) * 1.15 / speed)
            wander.timeOffset = randomTime(0...wander.duration)
            wander.repeatCount = .infinity
            wander.isRemovedOnCompletion = false
            wanderer.position = waypoints[0]
            wanderer.add(wander, forKey: "wander")

            // Curling: a small orbit, either way round, so the path loops and spirals.
            let radius = near ? random(8...18) : random(5...12)
            let orbit = CAKeyframeAnimation(keyPath: "position")
            let clockwise = Bool.random(using: &generator)
            var mirror = CGAffineTransform(scaleX: clockwise ? 1 : -1, y: 1)
            orbit.path = CGPath(ellipseIn: CGRect(x: -radius, y: -radius, width: radius * 2, height: radius * 2),
                                transform: &mirror)
            orbit.calculationMode = .paced
            orbit.duration = randomTime(2.4...5.5)
            orbit.timeOffset = randomTime(0...orbit.duration)
            orbit.repeatCount = .infinity
            orbit.isRemovedOnCompletion = false
            glow.add(orbit, forKey: "orbit")
        }
    }

    /// A glow in one colour: a white-hot centre, the colour, then a halo fading out.
    private static func glowColors(_ colour: UIColor) -> [CGColor] {
        let rgba = colour.rgba
        let core = UIColor(red: rgba.r * 0.4 + 0.6, green: rgba.g * 0.4 + 0.6, blue: rgba.b * 0.4 + 0.6, alpha: 1)
        return [core.cgColor, colour.cgColor, colour.withAlphaComponent(0.35).cgColor, colour.withAlphaComponent(0).cgColor]
    }

    /// An out-of-focus glow: an even, dim disc in one colour with a soft edge, and no hot centre.
    private static func bokehColors(_ colour: UIColor) -> [CGColor] {
        [0.3, 0.26, 0.12, 0].map { colour.withAlphaComponent($0).cgColor }
    }

    /// An animation that eases through `values` and back to the first, forever.
    private static func loop(_ keyPath: String, _ values: [Any], duration: Double, phase: Double) -> CAKeyframeAnimation {
        let animation = CAKeyframeAnimation(keyPath: keyPath)
        animation.values = values + [values[0]]
        animation.timingFunctions = values.map { _ in CAMediaTimingFunction(name: .easeInEaseOut) }
        animation.duration = duration
        animation.timeOffset = phase
        animation.repeatCount = .infinity
        animation.isRemovedOnCompletion = false
        return animation
    }

    /// A smooth closed curve through the points (Catmull-Rom turned into cubic Béziers).
    private static func closedCurve(through points: [CGPoint]) -> CGPath {
        let path = CGMutablePath()
        let n = points.count
        path.move(to: points[0])
        for i in 0..<n {
            let p0 = points[(i - 1 + n) % n], p1 = points[i], p2 = points[(i + 1) % n], p3 = points[(i + 2) % n]
            path.addCurve(to: p2,
                          control1: CGPoint(x: p1.x + (p2.x - p0.x) / 6, y: p1.y + (p2.y - p0.y) / 6),
                          control2: CGPoint(x: p2.x - (p3.x - p1.x) / 6, y: p2.y - (p3.y - p1.y) / 6))
        }
        path.closeSubpath()
        return path
    }

    /// The length of the closed polygon through the points (the curve is a little longer).
    private static func length(of points: [CGPoint]) -> CGFloat {
        points.indices.reduce(0) { total, i in
            let a = points[i], b = points[(i + 1) % points.count]
            return total + hypot(b.x - a.x, b.y - a.y)
        }
    }
}

private extension UIColor {
    var rgba: (r: CGFloat, g: CGFloat, b: CGFloat, a: CGFloat) {
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        getRed(&r, green: &g, blue: &b, alpha: &a)
        return (r, g, b, a)
    }
}

/// A small deterministic random generator, so layouts look the same on every launch.
struct SeededGenerator: RandomNumberGenerator {
    private var state: UInt64

    init(seed: UInt64) { state = seed &+ 0x9E37_79B9_7F4A_7C15 }

    mutating func next() -> UInt64 {
        state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
        var z = state
        z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
        z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
        return z ^ (z >> 31)
    }
}
