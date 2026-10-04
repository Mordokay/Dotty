import SwiftUI

/// The firefly lit in any colour: its glow and rim light are tinted, its body stays as it is.
struct TintedFirefly: View {
    var color: Color
    var size: CGFloat = 120

    var body: some View {
        ZStack {
            Image("FireflyLayerGlow").resizable().scaledToFit().colorMultiply(color)
            Image("FireflyLayerBody").resizable().scaledToFit()
            Image("FireflyLayerRim").resizable().scaledToFit().colorMultiply(color)
        }
        .frame(width: size, height: size)
        .accessibilityHidden(true)
    }
}

/// A colour picker made of jelly glass: a ring holding the whole spectrum, with the firefly in
/// the middle glowing in the chosen colour and breathing rings of that light outward. Any colour
/// is possible: the ring picks the hue and Richness goes from a whisper of colour (almost white)
/// to fully vivid; the bracelet's own brightness setting covers the rest of RGB. The light orbs
/// underneath are only shortcuts. The knob
/// is a swelling in the ring that travels around it like prey moving through a python: it lags
/// a little behind the finger, overshoots, and swells more the faster it is dragged.
struct FireflyColorWheel: View {
    @Binding var color: Color
    var size: CGFloat = 300

    @Environment(\.motionActive) private var active
    @Environment(\.renderQuality) private var quality
    @State private var knob = WheelKnob()
    @State private var dragging = false
    /// The finger has moved since touching down (a drag, not a tap): the swelling follows it exactly.
    @State private var following = false
    /// The ring's timeline runs only while the knob moves; at rest it is drawn once.
    @State private var awake = false
    /// 0 (almost white) ... 100 (fully vivid).
    @State private var richness = 72.0
    /// When the picked colour was last handed out while dragging. Handing it out on every move
    /// re-renders whatever shows it (the whole screen, often), so it goes out a few times a
    /// second and exactly on release.
    @State private var lastPublished: CFTimeInterval = 0

    private func color(at angle: Double) -> Color {
        Color(hue: Self.hue(at: angle), saturation: richness / 100, brightness: 1)
    }

    /// Which way the light is, from any point: up and a little left.
    private static let light = CGVector(dx: -0.5, dy: -0.866)

    private static func hue(at angle: Double) -> Double {
        let turn = angle / (2 * .pi)
        return turn - floor(turn)
    }

    private var radius: CGFloat { size / 2 - 34 }
    private let baseThickness: CGFloat = 31

    var body: some View {
        VStack(spacing: Spacing.xl) {
            ZStack {
                // Light breathing out of the firefly: rings that grow and fade on repeat (Core
                // Animation runs them; no timeline).
                EmanatingRings(color: color(at: knob.target), inner: radius * 0.3,
                               outer: radius - baseThickness / 2 - 4,
                               moving: active && !quality.isLow)
                TimelineView(.animation(paused: !awake)) { timeline in
                    let time = timeline.date.timeIntervalSinceReferenceDate
                    let _ = knob.advance(to: time, dragging: dragging, following: following)
                    wheel(time: time)
                }
            }
            .frame(width: size, height: size)
            .contentShape(Circle())
            .gesture(drag)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel("Light colour")
            .accessibilityValue(Self.colorName(at: knob.target))
            .accessibilityAdjustableAction { direction in
                let step = 2 * Double.pi / 24
                knob.setTarget(knob.target + (direction == .increment ? step : -step))
                wake()
                color = color(at: knob.target)
            }

            presets

            LightSlider(title: "Richness", value: $richness, range: 5...100,
                        light: Color(hue: Self.hue(at: knob.target), saturation: 1, brightness: 1),
                        emptyLight: Color(hue: Self.hue(at: knob.target), saturation: 0.05, brightness: 1))
                .frame(maxWidth: size)
                .padding(.horizontal, Spacing.l)
        }
        .onAppear {
            let components = Self.components(of: color)
            knob.jump(to: components.hue * 2 * .pi)
            richness = max(5, components.saturation * 100)
        }
        .onChange(of: richness) { _, _ in color = color(at: knob.target) }
    }

    // MARK: Drawing

    private func wheel(time: Double) -> some View {
        let center = CGPoint(x: size / 2, y: size / 2)
        let ring = WheelRing(center: center, radius: radius, thickness: baseThickness,
                             knobAngle: knob.angle, swell: knob.swell, trail: knob.trailOffset)
        let current = color(at: knob.angle)
        let knobPoint = CGPoint(x: center.x + radius * sin(knob.angle), y: center.y - radius * cos(knob.angle))
        let spectrum = AngularGradient(gradient: Gradient(colors: (0...12).map { color(at: Double($0) / 12 * 2 * .pi) }),
                                       center: .center, startAngle: .degrees(-90), endAngle: .degrees(270))

        let lump = (baseThickness + knob.swell) / 2
        // One light for the whole wheel, above the screen and a little to the left (the ring's
        // rim is brighter at the top for the same reason). The sheen sits on the bulge's side
        // facing it, wherever the bulge is on the ring.
        let shine = CGPoint(x: knobPoint.x + Self.light.dx * lump * 0.45, y: knobPoint.y + Self.light.dy * lump * 0.45)
        let knobUnit = UnitPoint(x: knobPoint.x / size, y: knobPoint.y / size)
        let litUnit = UnitPoint(x: (knobPoint.x + Self.light.dx * lump * 0.5) / size,
                                y: (knobPoint.y + Self.light.dy * lump * 0.5) / size)
        let glint = current.mix(with: .white, by: 0.4)

        return ZStack {
            // The picked colour spilling out around the bulge, so it stands out from the ring.
            GlowView(color: current, intensity: 0.55, core: 0.4, wobble: false)
                .frame(width: (lump + 34) * 2, height: (lump + 34) * 2)
                .position(knobPoint)

            // The ring: jelly glass with the spectrum glowing inside it, kept quieter than the
            // bulge so the picked colour is the brightest thing on it.
            Color.clear
                .glassEffect(.regular.tint(current.opacity(0.12)), in: ring)
            ring.fill(spectrum).opacity(0.4).drawingGroup().blendMode(.plusLighter)

            // The bulge and the rim light, rendered together on the GPU. Left to SwiftUI's default,
            // every gradient here is shaded pixel by pixel on the CPU across the whole wheel, every
            // frame the ring changes shape (a third of a frame each while it's held or turned).
            ZStack {
                // The swelling: the chosen colour pooled inside the bulge, whiter at its heart, with a
                // wet highlight. Painted normally (not added): additive light clips per channel, which
                // hazes yellows and draws hard rings in blues.
                // (Painted straight into the ring's shape, never masked: a mask costs an offscreen
                // pass every frame while the wheel turns.)
                ring.fill(RadialGradient(stops: poolStops(at: knob.angle), center: knobUnit,
                                         startRadius: 0, endRadius: lump + 14))
                // Its round edge turning away from the light, so it reads as a ball, not a slope.
                ring.fill(RadialGradient(stops: [.init(color: .black.opacity(0), location: 0.55),
                                                 .init(color: .black.opacity(0.22), location: 0.86),
                                                 .init(color: .black.opacity(0), location: 1)],
                                         center: knobUnit, startRadius: 0, endRadius: lump + 6))
                // A soft sheen where the light catches it: no edge, just white fading out.
                Ellipse()
                    .fill(EllipticalGradient(colors: [.white.opacity(0.42), .white.opacity(0.14), .white.opacity(0)],
                                             center: .center, startRadiusFraction: 0, endRadiusFraction: 0.5))
                    .frame(width: lump * 0.95, height: lump * 0.65)
                    .rotationEffect(.radians(atan2(Self.light.dy, Self.light.dx) + .pi / 2))
                    .position(shine)
                ring.stroke(LinearGradient(colors: [.white.opacity(0.35), .white.opacity(0.04)], startPoint: .top, endPoint: .bottom),
                            lineWidth: 1)
                // Light catching the bulge's edge: a thin glint in the picked colour, brightest on the
                // side facing the light (the gradient is centred off toward it) and fading along the ring.
                ring.stroke(RadialGradient(stops: [.init(color: glint, location: 0),
                                                   .init(color: glint.opacity(0.8), location: 0.42),
                                                   .init(color: glint.opacity(0.2), location: 0.72),
                                                   .init(color: glint.opacity(0), location: 1)],
                                           center: litUnit, startRadius: 0, endRadius: lump * 1.9),
                            lineWidth: 1.6)
            }
            .drawingGroup()

            TintedFirefly(color: current, size: radius * 1.1)
        }
        .frame(width: size, height: size)
    }

    /// The colour at half the picked richness in the centre (the same hue, whiter) deepening to
    /// the full richness outward. Only the outer part fades into the ring, so the pool has no
    /// edge: pure colours differ wildly in brightness (blue is far darker than yellow), and a
    /// solid rim would read as a disc in some hues.
    private func poolStops(at angle: Double) -> [Gradient.Stop] {
        let hue = Self.hue(at: angle), saturation = richness / 100
        func shade(_ fraction: Double, _ opacity: Double) -> Color {
            Color(hue: hue, saturation: saturation * fraction, brightness: 1).opacity(opacity)
        }
        return [
            .init(color: shade(0.5, 1), location: 0),
            .init(color: shade(0.62, 1), location: 0.25),
            .init(color: shade(0.82, 0.8), location: 0.5),
            .init(color: shade(1, 0.4), location: 0.75),
            .init(color: shade(1, 0), location: 1),
        ]
    }

    private var presets: some View {
        HStack(spacing: Spacing.m) {
            ForEach(DottyLight.allCases) { light in
                Button {
                    let components = Self.components(of: light.color)
                    knob.setTarget(knob.nearest(to: components.hue * 2 * .pi))
                    wake()
                    withAnimation(.settle) { richness = components.saturation * 100 }
                    color = light.color
                    Haptic.impact(0.4)
                } label: {
                    LightOrb(color: light.color, size: 30)
                        .padding(4)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(light.name)
            }
        }
    }

    // MARK: Interaction

    private var drag: some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { value in
                let dx = value.location.x - size / 2, dy = value.location.y - size / 2
                guard hypot(dx, dy) > radius * 0.35 else { return }
                if !dragging {
                    dragging = true
                    Haptic.impact(0.35)
                    wake()
                }
                if !following, hypot(value.translation.width, value.translation.height) > 6 {
                    following = true
                }
                let angle = atan2(Double(dx), Double(-dy))
                knob.setTarget(knob.nearest(to: angle))
                let now = CACurrentMediaTime()
                if now - lastPublished > 1.0 / 12 {
                    lastPublished = now
                    color = color(at: angle)
                }
            }
            .onEnded { _ in
                if dragging { color = color(at: knob.target) }
                dragging = false
                following = false
            }
    }

    /// Runs the ring's timeline until the knob settles again.
    private func wake() {
        guard !awake else { return }
        awake = true
        Task { @MainActor in
            repeat {
                try? await Task.sleep(for: .milliseconds(250))
            } while dragging || !knob.isSettled
            awake = false
        }
    }

    // MARK: Colour names and angles

    private static func components(of color: Color) -> (hue: Double, saturation: Double) {
        var hue: CGFloat = 0, saturation: CGFloat = 0, brightness: CGFloat = 0, alpha: CGFloat = 0
        UIColor(color).getHue(&hue, saturation: &saturation, brightness: &brightness, alpha: &alpha)
        return (Double(hue), Double(saturation))
    }

    private static func colorName(at angle: Double) -> String {
        let names = ["Red", "Orange", "Yellow", "Lime", "Green", "Mint", "Cyan", "Sky", "Blue", "Violet", "Magenta", "Pink"]
        return names[Int((hue(at: angle) * 12).rounded()) % 12]
    }
}

/// Rings of light breathing out of the firefly: each grows from the middle and fades, a third of
/// a breath apart, over a soft glow.
private struct EmanatingRings: View {
    let color: Color
    let inner: CGFloat
    let outer: CGFloat
    let moving: Bool

    var body: some View {
        ZStack {
            GlowView(color: color, intensity: 0.45, core: 0.5, wobble: false)
                .frame(width: outer * 2, height: outer * 2)
            if moving {
                ForEach(0..<3, id: \.self) { k in
                    Ring(color: color, inner: inner, outer: outer, delay: Durations.pulse * Double(k) / 3)
                }
            }
        }
        .allowsHitTesting(false)
    }

    private struct Ring: View {
        let color: Color
        let inner: CGFloat
        let outer: CGFloat
        let delay: Double
        @State private var grown = false

        var body: some View {
            ZStack {
                Circle().stroke(color.opacity(0.18), lineWidth: 14)
                Circle().stroke(color.opacity(0.45), lineWidth: 3)
            }
            .frame(width: outer * 2, height: outer * 2)
            .scaleEffect(grown ? 1 : inner / outer)
            .opacity(grown ? 0 : 1)
            .onAppear {
                withAnimation(.easeOut(duration: Durations.pulse).repeatForever(autoreverses: false).delay(delay)) {
                    grown = true
                }
            }
        }
    }
}

/// The knob's motion: a springy angle that lags and overshoots the finger, and a swelling that
/// grows while dragged and with speed, with a smaller lump trailing behind it.
final class WheelKnob {
    private(set) var angle = 0.0
    private(set) var target = 0.0
    private(set) var velocity = 0.0
    /// How much the ring swells at rest; dragging swells it far more.
    static let restingSwell = 24.0

    private(set) var swell = restingSwell
    private var swellVelocity = 0.0
    private var clock: Double?
    private var lastTick = 0

    /// Whether the knob has come to rest (so its timeline can pause).
    var isSettled: Bool {
        abs(velocity) < 0.002 && abs(target - angle) < 0.0005 && abs(swell - Self.restingSwell) < 0.05 && abs(swellVelocity) < 0.01
    }

    /// Where the trailing lump sits relative to the knob (radians), behind the motion.
    var trailOffset: Double { -max(-0.45, min(0.45, velocity * 0.05)) }

    func jump(to angle: Double) {
        self.angle = angle
        target = angle
        lastTick = Int(floor(angle / (2 * .pi) * 36))
    }

    func setTarget(_ angle: Double) { target = angle }

    /// `angle` unwrapped to the turn nearest the current target, so the knob takes the short way.
    func nearest(to angle: Double) -> Double {
        let turn = 2 * Double.pi
        var a = angle
        while a - target > .pi { a -= turn }
        while a - target < -.pi { a += turn }
        return a
    }

    func advance(to time: Double, dragging: Bool, following: Bool) {
        let dt = min(max(time - (clock ?? time), 0), 1.0 / 30)
        clock = time
        guard dt > 0 else { return }
        // While dragging, the swelling locks onto the finger (stiff, barely bouncy) so it always
        // points at it. Otherwise (a tap, a preset) it travels there with a little overshoot.
        let stiffness = following ? 1_400.0 : 240.0
        let damping = 2 * sqrt(stiffness) * (following ? 0.85 : 0.45)
        // Substeps keep the stiff following spring stable at any frame rate.
        let h = dt / 4
        for _ in 0..<4 {
            velocity += (stiffness * (target - angle) - damping * velocity) * h
            angle += velocity * h
        }

        let swellTarget = dragging ? 36 + min(14, abs(velocity) * 2.4) : Self.restingSwell
        swellVelocity += (160 * (swellTarget - swell) - 2 * sqrt(160) * 0.35 * swellVelocity) * dt
        swell += swellVelocity * dt

        // A soft tick every 10° the swelling travels, stronger when it moves fast.
        let tick = Int(floor(angle / (2 * .pi) * 36))
        if tick != lastTick {
            lastTick = tick
            Haptic.impact(0.15 + min(0.5, abs(velocity) * 0.05))
        }
    }
}

/// A ring whose thickness swells around the knob (and a smaller lump trailing it).
nonisolated struct WheelRing: Shape {
    var center: CGPoint
    var radius: CGFloat
    var thickness: CGFloat
    var knobAngle: Double
    var swell: Double
    var trail: Double

    func path(in rect: CGRect) -> Path {
        func thicknessAt(_ a: Double) -> CGFloat {
            func bump(_ at: Double, _ width: Double) -> Double {
                var d = (a - at).truncatingRemainder(dividingBy: 2 * .pi)
                if d > .pi { d -= 2 * .pi } else if d < -.pi { d += 2 * .pi }
                return exp(-(d * d) / (width * width))
            }
            return thickness + CGFloat(swell * bump(knobAngle, 0.19) + 0.45 * swell * bump(knobAngle + trail, 0.15) * min(1, abs(trail) * 4))
        }
        let samples = 180
        var path = Path()
        for i in 0...samples {
            let a = Double(i) / Double(samples) * 2 * .pi
            let r = radius + thicknessAt(a) / 2
            let p = CGPoint(x: center.x + r * sin(a), y: center.y - r * cos(a))
            i == 0 ? path.move(to: p) : path.addLine(to: p)
        }
        path.closeSubpath()
        for i in 0...samples {
            let a = Double(samples - i) / Double(samples) * 2 * .pi
            let r = radius - thicknessAt(a) / 2
            let p = CGPoint(x: center.x + r * sin(a), y: center.y - r * cos(a))
            i == 0 ? path.move(to: p) : path.addLine(to: p)
        }
        path.closeSubpath()
        return path
    }
}

#Preview {
    @Previewable @State var color = DottyLight.lagoon.color
    FireflyColorWheel(color: $color)
        .padding(40)
        .background(Color.void)
}
