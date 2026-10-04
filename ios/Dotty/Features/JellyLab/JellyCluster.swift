import SwiftUI

/// One jelly glass button in a cluster: its shape, where it rests, its light and its label.
struct JellySpec: Identifiable {
    let id: String
    var light: Color
    var size: CGSize
    var cornerRadius: CGFloat?
    /// Where the body's centre rests, in the cluster's coordinates.
    var center: CGPoint
    var label: AnyView
    var action: () -> Void
    /// Colour points for a gradient body (empty for a single light). Each point's richness
    /// swings gently, so the gradient is never quite still.
    var gradient: [Color] = []
    /// Runs the gradient top to bottom instead of left to right.
    var vertical = false
}

/// The jelly glass material: how rich its colour is and how opaque its body.
struct JellyMaterial: Equatable {
    /// 0...1: 1 keeps each light's full saturation; lower washes it toward white.
    var richness: Double = 1
    /// 0...1: how much colour fills the body, from clear glass to deeply tinted.
    var opacity: Double = 0.6
}

/// A group of jelly glass buttons sharing one physics world: Apple's Liquid Glass shaped by
/// soft bodies and lit from inside like salt lamps. Press one and light pours in where your
/// finger is while it dents and squashes; poke it and it wobbles; pull an edge and that edge
/// stretches out like playdough; push it into a neighbour and the neighbour is shoved aside,
/// their glass melting together and their lights mixing where they touch.
struct JellyCluster: View {
    let specs: [JellySpec]
    let size: CGSize
    var material = JellyMaterial()
    /// Whether a button can be pulled and stretched. Off, a touch only presses and pokes (the
    /// wiggle and the haptics), and scrolling over the buttons keeps working.
    var draggable = false

    /// How strongly labels follow the body: 1 is the plain best fit; more lets them stretch
    /// further with it.
    private let labelFollow: CGFloat = 1.7

    /// Room the cluster draws in beyond its frame, so stretched bodies and glows are never cut off.
    private let overflow: CGFloat = 150

    @Environment(\.motionActive) private var onScreen
    @Environment(\.renderQuality) private var quality
    @State private var world: JellyWorld
    @State private var awake = false
    @State private var active: Int?
    /// Where the touch began that let its button go to scroll, so the rest of that touch is
    /// ignored instead of grabbing the button again on its next move.
    @State private var abandonedTouch: CGPoint?
    @State private var lightPoint: CGPoint = .zero

    init(size: CGSize, material: JellyMaterial = JellyMaterial(), draggable: Bool = false, specs: [JellySpec]) {
        self.size = size
        self.specs = specs
        self.material = material
        self.draggable = draggable
        let offset = overflow
        let bodies = specs.map { spec in
            SoftBody(shape: .init(size: spec.size, cornerRadius: spec.cornerRadius ?? min(spec.size.width, spec.size.height) / 2),
                     center: CGPoint(x: spec.center.x + offset, y: spec.center.y + offset))
        }
        let world = JellyWorld(bodies: bodies)
        world.onContact = { strength in Haptic.impact(Double(0.25 + 0.5 * strength)) }
        _world = State(initialValue: world)
    }

    var body: some View {
        let frame = CGSize(width: size.width + overflow * 2, height: size.height + overflow * 2)
        // The physics timeline runs only while something moves; at rest nothing here redraws
        // (gradient bodies animate their fill on their own small timeline).
        TimelineView(.animation(paused: !awake)) { timeline in
            let time = timeline.date.timeIntervalSinceReferenceDate
            let _ = world.advance(to: time)
            let layers = specs.indices.map { layer(index: $0, time: time) }
            ZStack(alignment: .topLeading) {
                // A salt lamp sheds only a small, soft light around itself.
                ForEach(layers) { layer in
                    GlowView(color: layer.light, intensity: 0.16 + 0.3 * Double(layer.energy), core: 0.6, wobble: false)
                        .frame(width: layer.size.width + 70, height: layer.size.height + 70)
                        .position(layer.centroid)
                }

                // Liquid Glass, reshaped by the simulation every frame. Only the glass goes in the
                // container (it lenses whatever is inside it); shapes within a few points of each
                // other melt together.
                GlassEffectContainer(spacing: 14) {
                    ZStack(alignment: .topLeading) {
                        ForEach(layers) { layer in
                            Color.clear
                                .frame(width: frame.width, height: frame.height)
                                .glassEffect(.regular.tint(layer.light.opacity(0.1 + 0.35 * material.opacity + 0.2 * layer.energy)), in: layer.outline)
                        }
                    }
                }

                // The body's own colour: a deeper tint inside the glass, as opaque as the material
                // asks, so white text stays readable on it.
                ForEach(layers) { layer in
                    bodyColor(layer, frame: frame)
                }

                // Light inside each body: it pools where it is pushed in and escapes where the body
                // is thin. Clipped to the body, so lights only add up where bodies actually meet.
                ForEach(layers) { layer in
                    innerLight(layer, frame: frame)
                }

                // Labels ride the body: they take its best-fit stretch, shear and turn, so they
                // stay inside it however it is pulled.
                ForEach(layers) { layer in
                    specs[layer.index].label
                        .foregroundStyle(Color.ink)
                        .frame(width: layer.size.width, height: layer.size.height)
                        .transformEffect(layer.labelTransform)
                }
            }
            .frame(width: frame.width, height: frame.height, alignment: .topLeading)
            .allowsHitTesting(false)
        }
        .frame(width: frame.width, height: frame.height, alignment: .topLeading)
        .contentShape(Rectangle().inset(by: overflow))
        .modifier(TouchModifier(gesture: touch, exclusive: draggable))
        .padding(-overflow)
        .accessibilityElement(children: .ignore)
        .accessibilityChildren {
            ForEach(specs) { spec in
                Button(action: spec.action) { spec.label }
            }
        }
    }

    /// What one body looks like this frame.
    private struct Layer: Identifiable {
        let index: Int
        let outline: JellyOutline
        let light: Color
        let energy: CGFloat
        let strain: SIMD3<Float>
        let centroid: CGPoint
        let pour: CGPoint
        let size: CGSize
        let labelTransform: CGAffineTransform
        /// The body's colour: one deeper shade of its light, or (for gradients) the points.
        let tint: Color
        let gradient: [Color]
        let vertical: Bool
        var id: Int { index }
    }

    private func layer(index: Int, time: Double) -> Layer {
        let body = world.bodies[index]
        let spec = specs[index]
        let centroid = CGPoint(x: CGFloat(body.centroid.x), y: CGFloat(body.centroid.y))
        // Maps the label's box (origin at its top-left) onto the deformed body, following the
        // body's stretch more than the plain best fit so the text visibly goes with it.
        let m = body.deformation
        let half = CGSize(width: spec.size.width / 2, height: spec.size.height / 2)
        let follow = labelFollow
        let a = 1 + (CGFloat(m.a) - 1) * follow, b = CGFloat(m.b) * follow
        let c = CGFloat(m.c) * follow, d = 1 + (CGFloat(m.d) - 1) * follow

        let richness = material.richness
        let light = spec.gradient.isEmpty ? spec.light.withRichness(richness) : spec.gradient[spec.gradient.count / 2].withRichness(richness)
        // The body is a deeper shade of its light, so the glow on top reads as light inside it.
        // Gradients are deepened less, so their colours stay vivid.
        let tint = spec.light.withRichness(richness).mix(with: .black, by: 0.38)
        let gradient = spec.gradient.map { $0.withRichness(richness).mix(with: .black, by: 0.15) }
        let transform = CGAffineTransform(a: a, b: b, c: c, d: d,
                                          tx: centroid.x - (a * half.width + c * half.height),
                                          ty: centroid.y - (b * half.width + d * half.height))
        return Layer(index: index,
                     outline: JellyOutline(points: body.points.map { CGPoint(x: CGFloat($0.x), y: CGFloat($0.y)) }),
                     light: light,
                     energy: CGFloat(body.energy),
                     strain: body.strain,
                     centroid: centroid,
                     pour: index == active ? lightPoint : centroid,
                     size: spec.size,
                     labelTransform: transform,
                     tint: tint,
                     gradient: gradient,
                     vertical: spec.vertical)
    }

    @ViewBuilder
    private func bodyColor(_ layer: Layer, frame: CGSize) -> some View {
        if layer.gradient.isEmpty {
            layer.outline
                .fill(layer.tint)
                .opacity(0.15 + 0.6 * material.opacity)
        } else {
            // Only this fill keeps moving at rest: its colour points swing gently, at 20 fps.
            TimelineView(.animation(minimumInterval: 1.0 / 20, paused: !onScreen || quality.isLow)) { timeline in
                let stops = GradientButtonStyle.stops(colors: layer.gradient, time: timeline.date.timeIntervalSinceReferenceDate)
                let reach = layer.vertical ? layer.size.height / 2 : layer.size.width / 2
                let c = layer.centroid
                layer.outline
                    .fill(LinearGradient(stops: stops,
                                         startPoint: UnitPoint(x: (layer.vertical ? c.x : c.x - reach) / frame.width,
                                                               y: (layer.vertical ? c.y - reach : c.y) / frame.height),
                                         endPoint: UnitPoint(x: (layer.vertical ? c.x : c.x + reach) / frame.width,
                                                             y: (layer.vertical ? c.y + reach : c.y) / frame.height)))
                    .opacity(0.35 + 0.6 * material.opacity)
            }
        }
    }

    private func innerLight(_ layer: Layer, frame: CGSize) -> some View {
        Canvas { context, _ in
            let path = layer.outline.path(in: CGRect(origin: .zero, size: frame))
            context.clip(to: path)
            // Light escaping at the thin edges: soft strokes instead of a blur filter.
            let edgeLight = 0.38 + 0.5 * layer.energy
            context.stroke(path, with: .color(layer.light.opacity(edgeLight * 0.35)), lineWidth: 18)
            context.stroke(path, with: .color(layer.light.opacity(edgeLight * 0.5)), lineWidth: 8)
            let pool = 0.12 + 0.88 * layer.energy
            let radius = min(layer.size.width, layer.size.height) * (1.1 + 0.6 * layer.energy)
            context.fill(path, with: .radialGradient(
                Gradient(colors: [Color.white.opacity(0.55 * pool), layer.light.opacity(0.6 * pool), layer.light.opacity(0)]),
                center: layer.pour, startRadius: 0, endRadius: radius))
        }
        .frame(width: frame.width, height: frame.height)
        .blendMode(.plusLighter)
    }

    private var touch: some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { drag in
                let now = Date.timeIntervalSinceReferenceDate
                if drag.startLocation == abandonedTouch { return }
                if active == nil {
                    guard let hit = world.body(at: drag.startLocation) else { return }
                    active = hit
                    world.bodies[hit].press(at: drag.startLocation, time: now)
                    Haptic.impact(0.35)
                } else if let active {
                    if draggable {
                        world.bodies[active].drag(to: drag.location, time: now)
                    } else if hypot(drag.translation.width, drag.translation.height) > 12 {
                        // Not draggable: moving the finger means scrolling, so let the button go.
                        world.bodies[active].release(at: drag.startLocation, time: now)
                        self.active = nil
                        abandonedTouch = drag.startLocation
                        sleepWhenSettled()
                        return
                    }
                }
                lightPoint = drag.location
                awake = true
            }
            .onEnded { drag in
                guard let index = active else { return }
                let now = Date.timeIntervalSinceReferenceDate
                let squash = world.bodies[index].release(at: drag.location, time: now)
                Haptic.impact(Double(0.3 + 0.7 * squash))
                active = nil
                let moved = hypot(drag.translation.width, drag.translation.height)
                if moved < 12, world.body(at: drag.location) == index {
                    specs[index].action()
                }
                sleepWhenSettled()
            }
    }

    /// Pauses the timeline once every body has settled, so an idle cluster costs nothing.
    private func sleepWhenSettled() {
        Task { @MainActor in
            while !world.isAsleep || active != nil {
                try? await Task.sleep(for: .milliseconds(250))
            }
            awake = false
        }
    }
}

/// Draggable clusters own their touches; the others share them with scrolling.
private struct TouchModifier<G: Gesture>: ViewModifier {
    let gesture: G
    let exclusive: Bool

    func body(content: Content) -> some View {
        if exclusive {
            content.gesture(gesture)
        } else {
            content.simultaneousGesture(gesture)
        }
    }
}

/// A smooth closed outline through the simulation's points (Catmull-Rom, as cubic curves).
nonisolated struct JellyOutline: Shape {
    var points: [CGPoint]

    func path(in rect: CGRect) -> Path {
        var path = Path()
        let n = points.count
        guard n > 2 else { return path }
        path.move(to: points[0])
        for i in 0..<n {
            let p0 = points[(i + n - 1) % n], p1 = points[i], p2 = points[(i + 1) % n], p3 = points[(i + 2) % n]
            let c1 = CGPoint(x: p1.x + (p2.x - p0.x) / 6, y: p1.y + (p2.y - p0.y) / 6)
            let c2 = CGPoint(x: p2.x - (p3.x - p1.x) / 6, y: p2.y - (p3.y - p1.y) / 6)
            path.addCurve(to: p2, control1: c1, control2: c2)
        }
        path.closeSubpath()
        return path
    }
}
