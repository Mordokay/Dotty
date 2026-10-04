import SwiftUI
import UIKit

/// An on/off switch whose knob becomes a small light when on.
struct LightToggleStyle: ToggleStyle {
    var light: Color = DottyLight.firefly.color

    func makeBody(configuration: Configuration) -> some View {
        let on = configuration.isOn
        HStack(spacing: Spacing.m) {
            configuration.label
                .font(.lpHeadline)
                .foregroundStyle(Color.ink)
            Spacer(minLength: Spacing.m)
            Capsule()
                .fill(on ? light.opacity(0.32) : Color.glassStrong)
                .overlay(Capsule().strokeBorder(on ? light.opacity(0.45) : .white.opacity(0.06), lineWidth: 1))
                .frame(width: 58, height: 34)
                // When on, the whole switch glows, not just its knob.
                .background { ShapeGlow(shape: Capsule(), color: light, reach: 12, intensity: 0.5).opacity(on ? 1 : 0) }
                .overlay(alignment: on ? .trailing : .leading) {
                    Circle()
                        .fill(on
                              ? AnyShapeStyle(RadialGradient(colors: [.white, light, light.mix(with: .black, by: 0.3)],
                                                             center: UnitPoint(x: 0.4, y: 0.35), startRadius: 0, endRadius: 16))
                              : AnyShapeStyle(RadialGradient(colors: [.white, Color(hex: 0xC9CCD6), Color(hex: 0x9AA0B0)],
                                                             center: UnitPoint(x: 0.4, y: 0.35), startRadius: 0, endRadius: 16)))
                        .frame(width: 26, height: 26)
                        .background {
                            if on { GlowView(color: light, intensity: 0.6, core: 0.45, wobble: false).padding(-8) }
                        }
                        .padding(4)
                }
        }
        .contentShape(Rectangle())
        .onTapGesture {
            withAnimation(.settle) { configuration.isOn.toggle() }
        }
        .sensoryFeedback(Haptic.select, trigger: on)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
        .accessibilityValue(on ? "On" : "Off")
    }
}

extension ToggleStyle where Self == LightToggleStyle {
    static func light(_ color: Color = DottyLight.firefly.color) -> LightToggleStyle { LightToggleStyle(light: color) }
}

/// A slider made of light. The fill is a gradient across the whole track, from `emptyLight`
/// at empty to `light` at full, so the colour itself tells the level: as it fills, the thumb
/// takes the colour of where it is. The thumb grows and glows brighter toward full, each step's
/// haptic gets stronger toward full, and while dragging the thumb leaves a short comet trail.
struct LightSlider: View {
    let title: String
    @Binding var value: Double
    var range: ClosedRange<Double> = 0...100
    var step: Double = 1
    /// The light at full.
    var light: Color = DottyLight.firefly.color
    /// The light at empty. Defaults to a contrasting hue of `light`.
    var emptyLight: Color?
    var format: (Double) -> String = { "\(Int($0))%" }

    @State private var dragging = false
    @State private var trail: [TrailPoint] = []
    /// A tap far along the track glides the thumb there instead of jumping.
    @State private var glide: Glide?
    @State private var sparks: [Spark] = []
    @State private var lastDrag: (fraction: Double, time: Date)?
    @Environment(\.sliderSparks) private var sparksOn
    @State private var hapticBucket = -1

    private struct TrailPoint {
        var fraction: Double
        var time: Date
    }

    /// A spark thrown off the thumb in a fast drag: it flies out, arcs down and fades.
    private struct Spark {
        var fraction: Double
        var velocity: CGVector
        var size: CGFloat
        var born: Date
    }

    private static let sparkLife = 0.6

    private struct Glide {
        var from: Double
        var to: Double
        var start: Date
    }

    private static let glideDuration = 0.3

    /// Where the thumb is drawn: the value, or on its way there during a glide.
    private func shownFraction(at time: Date) -> Double {
        guard let glide else { return fraction }
        let progress = min(1, max(0, time.timeIntervalSince(glide.start) / Self.glideDuration))
        let eased = 1 - pow(1 - progress, 3)
        return glide.from + (glide.to - glide.from) * eased
    }

    private static let trailLife = 0.35
    private static let thumb: CGFloat = 30

    private var fraction: Double { (value - range.lowerBound) / (range.upperBound - range.lowerBound) }
    private var from: Color { emptyLight ?? light.complement }
    private func color(at f: Double) -> Color { from.hueMix(with: light, by: f) }

    var body: some View {
        VStack(spacing: Spacing.s) {
            HStack {
                Text(title).font(.lpHeadline).foregroundStyle(Color.ink)
                Spacer()
                Text(format(value))
                    .font(.lpCallout)
                    .monospacedDigit()
                    .foregroundStyle(color(at: fraction).mix(with: .ink, by: 0.35))
            }
            GeometryReader { geometry in
                let width = geometry.size.width
                TimelineView(.animation(paused: !dragging && trail.isEmpty && glide == nil && sparks.isEmpty)) { timeline in
                    track(width: width, now: timeline.date)
                }
                .contentShape(Rectangle())
                .gesture(DragGesture(minimumDistance: 0)
                    .onChanged { drag in
                        let now = Date.now
                        let f = Double(min(max((drag.location.x - Self.thumb / 2) / (width - Self.thumb), 0), 1))
                        if !dragging {
                            withAnimation(.spring(response: 0.2, dampingFraction: 0.7)) { dragging = true }
                            if abs(f - fraction) > 0.05 {
                                // A tap far away: glide there in 0.3 s, leaving a trail.
                                let start = now
                                glide = Glide(from: fraction, to: f, start: start)
                                Task { @MainActor in
                                    try? await Task.sleep(for: .seconds(Self.glideDuration))
                                    if glide?.start == start { glide = nil }
                                }
                            }
                        } else if glide != nil {
                            glide?.to = f
                        }
                        let raw = range.lowerBound + f * (range.upperBound - range.lowerBound)
                        value = (raw / step).rounded() * step
                        if glide == nil {
                            trail.append(TrailPoint(fraction: fraction, time: now))
                            throwSparks(at: f, time: now)
                        }
                        lastDrag = (f, now)
                        trail.removeAll { now.timeIntervalSince($0.time) > Self.trailLife }
                    }
                    .onEnded { _ in
                        withAnimation(.settle) { dragging = false }
                        lastDrag = nil
                        Task { @MainActor in
                            try? await Task.sleep(for: .seconds(max(Self.trailLife, Self.sparkLife)))
                            if !dragging {
                                trail.removeAll()
                                sparks.removeAll()
                            }
                        }
                    })
            }
            .frame(height: 40)
        }
        .onChange(of: value) { _, _ in
            // A light tick every 2%, stronger toward full, so the level can be felt.
            let bucket = Int(fraction * 50)
            if bucket != hapticBucket {
                hapticBucket = bucket
                Haptic.impact(0.2 + 0.8 * fraction)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(title)
        .accessibilityValue(format(value))
        .accessibilityAdjustableAction { direction in
            switch direction {
            case .increment: value = min(range.upperBound, value + step * 5)
            case .decrement: value = max(range.lowerBound, value - step * 5)
            @unknown default: break
            }
        }
    }

    /// Throws a few sparks off the thumb when it is dragged fast (if sparks are on).
    private func throwSparks(at f: Double, time: Date) {
        guard sparksOn, let last = lastDrag else { return }
        let dt = time.timeIntervalSince(last.time)
        guard dt > 0.001 else { return }
        let speed = (f - last.fraction) / dt
        guard abs(speed) > 0.6 else { return }
        let count = min(3, Int(abs(speed) * 1.5))
        let away: CGFloat = speed > 0 ? -1 : 1
        for _ in 0..<count {
            sparks.append(Spark(fraction: f,
                                velocity: CGVector(dx: away * .random(in: 20...110), dy: .random(in: -150 ... -40)),
                                size: .random(in: 1.2...2.6),
                                born: time))
        }
        sparks.removeAll { time.timeIntervalSince($0.born) > Self.sparkLife }
        if sparks.count > 48 { sparks.removeFirst(sparks.count - 48) }
    }

    private func track(width: CGFloat, now: Date) -> some View {
        let shown = shownFraction(at: now)
        let f = CGFloat(shown)
        let travel = width - Self.thumb
        let x = f * travel + Self.thumb / 2
        let current = color(at: shown)
        // During a glide the trail is the glide's own path, sampled a little earlier each step.
        let glideTrail: [TrailPoint] = glide.map { glide in
            (1...14).compactMap { k in
                let time = now.addingTimeInterval(-Double(k) * 0.02)
                return time >= glide.start ? TrailPoint(fraction: shownFraction(at: time), time: time) : nil
            }
        } ?? []
        // Several stops along the colour wheel, so the middle stays vivid instead of greying out.
        let stops = (0...8).map { i in Gradient.Stop(color: color(at: Double(i) / 8), location: Double(i) / 8) }
        let gradient = LinearGradient(stops: stops, startPoint: .leading, endPoint: .trailing)
        let thumbSize = Self.thumb * (0.85 + 0.25 * f) * (dragging ? 1.1 : 1)

        return ZStack(alignment: .leading) {
            // The empty track hints at the colours to come.
            Capsule().fill(Color.glassStrong).frame(height: 10)
            Capsule().fill(gradient).opacity(0.14).frame(height: 10)

            // The fill: the gradient revealed up to the thumb, glowing more as it fills.
            Capsule()
                .fill(gradient)
                .frame(height: 10)
                .mask(alignment: .leading) { Capsule().frame(width: max(10, x)) }
            // A soft fade of light along the fill, stronger toward full.
            Capsule()
                .fill(LinearGradient(colors: [from.opacity(0), current.opacity(0.18 + 0.3 * f)], startPoint: .leading, endPoint: .trailing))
                .frame(width: max(10, x), height: 26)

            // The comet trail: one tapered Bézier shape from where the thumb was a moment ago to
            // where it is, fading toward the tail. Two plain fills, no blur filter.
            Canvas { context, canvasSize in
                let recent = (trail + glideTrail).filter { now.timeIntervalSince($0.time) < Self.trailLife }
                guard let tail = recent.min(by: { $0.time < $1.time }) else { return }
                let headX = CGFloat(shown) * travel + Self.thumb / 2
                let tailX = CGFloat(tail.fraction) * travel + Self.thumb / 2
                guard abs(headX - tailX) > 4 else { return }
                let y = canvasSize.height / 2
                let gradient = Gradient(colors: [color(at: tail.fraction).opacity(0), color(at: shown)])
                for (width, opacity) in [(thumbSize * 0.9, 0.3), (thumbSize * 0.45, 0.75)] {
                    let half = width / 2
                    var comet = Path()
                    comet.move(to: CGPoint(x: tailX, y: y))
                    comet.addQuadCurve(to: CGPoint(x: headX, y: y - half), control: CGPoint(x: (tailX + headX) / 2, y: y - half * 0.5))
                    comet.addLine(to: CGPoint(x: headX, y: y + half))
                    comet.addQuadCurve(to: CGPoint(x: tailX, y: y), control: CGPoint(x: (tailX + headX) / 2, y: y + half * 0.5))
                    comet.closeSubpath()
                    var layer = context
                    layer.opacity = opacity
                    layer.blendMode = .plusLighter
                    layer.fill(comet, with: .linearGradient(gradient, startPoint: CGPoint(x: tailX, y: y), endPoint: CGPoint(x: headX, y: y)))
                }
            }
            .allowsHitTesting(false)

            // Sparks, when switched on: thrown off the thumb in fast drags.
            if !sparks.isEmpty {
                Canvas { context, canvasSize in
                    let y = canvasSize.height / 2
                    context.blendMode = .plusLighter
                    for spark in sparks {
                        let age = now.timeIntervalSince(spark.born)
                        guard age >= 0, age < Self.sparkLife else { continue }
                        let t = CGFloat(age)
                        let x = CGFloat(spark.fraction) * travel + Self.thumb / 2 + spark.velocity.dx * t
                        let sparkY = y + spark.velocity.dy * t + 0.5 * 320 * t * t
                        let fade = 1 - age / Self.sparkLife
                        let r = spark.size * CGFloat(0.4 + 0.6 * fade)
                        let tint = color(at: spark.fraction)
                        context.fill(Path(ellipseIn: CGRect(x: x - r * 3, y: sparkY - r * 3, width: r * 6, height: r * 6)),
                                     with: .radialGradient(Gradient(colors: [tint.opacity(0.5 * fade), tint.opacity(0)]),
                                                           center: CGPoint(x: x, y: sparkY), startRadius: 0, endRadius: r * 3))
                        context.fill(Path(ellipseIn: CGRect(x: x - r, y: sparkY - r, width: r * 2, height: r * 2)),
                                     with: .color(tint.mix(with: .white, by: 0.6).opacity(fade)))
                    }
                }
                // Room above and below for sparks to fly (a Canvas clips to its frame).
                .padding(.vertical, -60)
                .allowsHitTesting(false)
            }

            // The thumb: a small light in the colour of its level.
            Circle()
                .fill(RadialGradient(colors: [.white, current.mix(with: .white, by: 0.45), current],
                                     center: UnitPoint(x: 0.4, y: 0.35), startRadius: 0, endRadius: thumbSize * 0.6))
                .frame(width: thumbSize, height: thumbSize)
                // A gradient halo that grows and brightens with the level.
                .background {
                    GlowView(color: current, intensity: 0.45 + 0.35 * f + (dragging ? 0.15 : 0), core: 0.5, wobble: false)
                        .padding(-(10 + 16 * f))
                }
                .offset(x: x - thumbSize / 2)
        }
        .frame(maxHeight: .infinity)
    }
}

extension Color {
    private var hsba: (h: Double, s: Double, b: Double, a: Double) {
        var hue: CGFloat = 0, saturation: CGFloat = 0, brightness: CGFloat = 0, alpha: CGFloat = 0
        UIColor(self).getHue(&hue, saturation: &saturation, brightness: &brightness, alpha: &alpha)
        return (hue, saturation, brightness, alpha)
    }

    /// The same hue at `richness` (0...1) of its saturation: 1 keeps it, lower washes it toward white.
    func withRichness(_ richness: Double) -> Color {
        let c = hsba
        return Color(hue: c.h, saturation: c.s * richness, brightness: c.b, opacity: c.a)
    }

    /// A contrasting hue (150° around the wheel), at the same saturation and brightness.
    var complement: Color {
        let c = hsba
        return Color(hue: (c.h + 150.0 / 360).truncatingRemainder(dividingBy: 1), saturation: c.s, brightness: c.b, opacity: c.a)
    }

    /// Blends toward `other` around the colour wheel (the shorter way), as light does,
    /// instead of through grey.
    func hueMix(with other: Color, by t: Double) -> Color {
        let a = hsba, b = other.hsba
        var delta = b.h - a.h
        if delta > 0.5 { delta -= 1 } else if delta < -0.5 { delta += 1 }
        let hue = (a.h + delta * t + 1).truncatingRemainder(dividingBy: 1)
        return Color(hue: hue, saturation: a.s + (b.s - a.s) * t, brightness: a.b + (b.b - a.b) * t, opacity: a.a + (b.a - a.a) * t)
    }
}

/// The bracelet's colours as small light sources. The chosen one swells and glows.
struct LightColorPicker: View {
    @Binding var selection: DottyLight

    var body: some View {
        HStack(spacing: Spacing.m) {
            ForEach(DottyLight.allCases) { light in
                let selected = light == selection
                Button {
                    withAnimation(.settle) { selection = light }
                } label: {
                    LightOrb(color: light.color, size: 40)
                        .padding(2)
                        .overlay {
                            Circle()
                                .strokeBorder(light.color.mix(with: .white, by: 0.3), lineWidth: 2)
                                .padding(-3)
                                .opacity(selected ? 1 : 0)
                        }
                        .scaleEffect(selected ? 1.12 : 0.92)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(light.name)
                .accessibilityAddTraits(selected ? .isSelected : [])
            }
        }
        .sensoryFeedback(Haptic.select, trigger: selection)
    }
}
