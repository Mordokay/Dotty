import SwiftUI
import UIKit

/// The one primary action on a screen: filled with light and glowing in its colour.
struct LightButtonStyle: ButtonStyle {
    var light: Color = DottyLight.firefly.color
    var circle = false
    /// Off when the label sizes itself (MorphButton).
    var padded = true

    func makeBody(configuration: Configuration) -> some View {
        let pressed = configuration.isPressed
        configuration.label
            .font(.lpHeadline)
            .foregroundStyle(Color.ink)
            .padding(.horizontal, circle || !padded ? 0 : Spacing.xl)
            .frame(minWidth: circle ? 52 : nil, minHeight: 52)
            .frame(width: circle ? 52 : nil)
            .background {
                // Lit glass: rich colour over a dark veil, brighter where the light enters, so
                // white text stays readable on every light colour.
                ZStack {
                    ButtonShape(circle: circle).fill(.ultraThinMaterial)
                    ButtonShape(circle: circle).fill(Color.glassVeil)
                    ButtonShape(circle: circle)
                        .fill(RadialGradient(colors: [light.mix(with: .white, by: 0.15).opacity(0.85), light.opacity(0.62), light.mix(with: .black, by: 0.35).opacity(0.7)],
                                             center: UnitPoint(x: 0.3, y: 0), startRadius: 0, endRadius: 150))
                }
                .overlay {
                    ButtonShape(circle: circle)
                        .strokeBorder(LinearGradient(colors: [light.mix(with: .white, by: 0.6), .clear], startPoint: .top, endPoint: .center), lineWidth: 1)
                }
            }
            // A light, softly wobbling glow (a shader, far cheaper than blurred shadows).
            .background {
                GlowView(color: light, intensity: pressed ? 0.42 : 0.24, core: 0.62, wobble: false)
                    .padding(-26)
            }
            .scaleEffect(pressed ? 0.95 : 1)
            .animation(.spring(response: 0.25, dampingFraction: 0.6), value: pressed)
            .sensoryFeedback(trigger: pressed) { _, isPressed in isPressed ? Haptic.tap : nil }
    }
}

/// Every other action: frosted glass.
struct FrostedButtonStyle: ButtonStyle {
    var circle = false

    func makeBody(configuration: Configuration) -> some View {
        let pressed = configuration.isPressed
        configuration.label
            .font(.lpHeadline)
            .foregroundStyle(Color.ink)
            .padding(.horizontal, circle ? 0 : Spacing.xl)
            .frame(minHeight: 52)
            .frame(width: circle ? 52 : nil)
            .background {
                ZStack {
                    ButtonShape(circle: circle).fill(.ultraThinMaterial)
                    ButtonShape(circle: circle).fill(Color.glassVeil)
                    ButtonShape(circle: circle).fill(pressed ? Color.glassStrong : Color.glass)
                }
                .overlay {
                    ButtonShape(circle: circle)
                        .strokeBorder(LinearGradient(colors: [.glassEdge, .white.opacity(0.03)], startPoint: .top, endPoint: .center), lineWidth: 1)
                }
            }
            .scaleEffect(pressed ? 0.95 : 1)
            .animation(.spring(response: 0.25, dampingFraction: 0.6), value: pressed)
            .sensoryFeedback(trigger: pressed) { _, isPressed in isPressed ? Haptic.tap : nil }
    }
}

/// Low-emphasis actions next to others: text in a light colour, with no background ever.
/// A press makes the light pulse: the glow swells and the text grows to 1.3× and back.
struct QuietButtonStyle: ButtonStyle {
    var light: Color = DottyLight.firefly.color

    func makeBody(configuration: Configuration) -> some View {
        QuietButtonBody(label: configuration.label, isPressed: configuration.isPressed, light: light)
    }
}

private struct QuietButtonBody<Label: View>: View {
    let label: Label
    let isPressed: Bool
    let light: Color

    @State private var pulses = 0

    var body: some View {
        label
            .font(.lpHeadline)
            .foregroundStyle(light)
            .padding(.horizontal, Spacing.l)
            .frame(minHeight: 44)
            .contentShape(Capsule())
            .keyframeAnimator(initialValue: 0.0, trigger: pulses) { content, pulse in
                content
                    .scaleEffect(1 + 0.3 * pulse)
                    .background {
                        GlowView(color: light, intensity: 0.55, core: 0.5, wobble: false)
                            .padding(-18)
                            .opacity(pulse)
                    }
            } keyframes: { _ in
                KeyframeTrack {
                    CubicKeyframe(1, duration: 0.18)
                    SpringKeyframe(0, duration: 0.45, spring: .init(response: 0.45, dampingRatio: 0.55))
                }
            }
            .onChange(of: isPressed) { _, pressed in
                if pressed { pulses += 1 }
            }
            .sensoryFeedback(trigger: isPressed) { _, pressed in pressed ? Haptic.tap : nil }
    }
}

/// A light button filled with a moving gradient: several colour points whose richness swings
/// like a pendulum between 90% and 100%, and drift a little along the gradient, so it is never
/// quite still. `vertical` runs the gradient top to bottom instead of left to right.
struct GradientButtonStyle: ButtonStyle {
    var colors: [Color]
    var vertical = false

    func makeBody(configuration: Configuration) -> some View {
        GradientButtonBody(configuration: configuration, colors: colors, vertical: vertical)
    }

    static func stops(colors: [Color], time: Double) -> [Gradient.Stop] {
        let count = colors.count
        return colors.enumerated().map { index, color in
            // Each point swings between 90% and 100% of its richness, out of step with the others.
            let swing = 0.9 + 0.1 * (0.5 + 0.5 * sin(time * 1.1 + Double(index) * 1.7))
            var hue: CGFloat = 0, saturation: CGFloat = 0, brightness: CGFloat = 0, alpha: CGFloat = 0
            UIColor(color).getHue(&hue, saturation: &saturation, brightness: &brightness, alpha: &alpha)
            let base = count > 1 ? Double(index) / Double(count - 1) : 0
            let drift = (index == 0 || index == count - 1) ? 0 : 0.06 * sin(time * 0.6 + Double(index) * 2.3)
            return Gradient.Stop(color: Color(hue: hue, saturation: saturation * swing, brightness: brightness),
                                 location: base + drift)
        }
    }
}

private struct GradientButtonBody: View {
    let configuration: ButtonStyleConfiguration
    let colors: [Color]
    let vertical: Bool

    @Environment(\.motionActive) private var active
    @Environment(\.renderQuality) private var quality

    var body: some View {
        let pressed = configuration.isPressed
        TimelineView(.animation(minimumInterval: 1.0 / 20, paused: !active || quality.isLow)) { timeline in
            let time = timeline.date.timeIntervalSinceReferenceDate
            let stops = GradientButtonStyle.stops(colors: colors, time: time)
            let middle = colors[colors.count / 2]
            configuration.label
                .font(.lpHeadline)
                .foregroundStyle(Color.onLight)
                .padding(.horizontal, Spacing.xl)
                .frame(minHeight: 52)
                .background {
                    Capsule()
                        .fill(LinearGradient(stops: stops,
                                             startPoint: vertical ? .top : .leading,
                                             endPoint: vertical ? .bottom : .trailing))
                        .overlay {
                            Capsule().strokeBorder(LinearGradient(colors: [.white.opacity(0.55), .clear], startPoint: .top, endPoint: .center), lineWidth: 1)
                        }
                }
                .shadow(color: middle.opacity(pressed ? 0.65 : 0.45), radius: pressed ? 20 : 14)
                .shadow(color: (colors.first ?? middle).opacity(0.18), radius: 36, x: vertical ? 0 : -12, y: vertical ? -10 : 0)
                .shadow(color: (colors.last ?? middle).opacity(0.18), radius: 36, x: vertical ? 0 : 12, y: vertical ? 10 : 0)
        }
        .scaleEffect(pressed ? 0.95 : 1)
        .animation(.spring(response: 0.25, dampingFraction: 0.6), value: pressed)
        .sensoryFeedback(trigger: pressed) { _, isPressed in isPressed ? Haptic.tap : nil }
    }
}

/// A pill, or a circle for icon buttons.
nonisolated private struct ButtonShape: InsettableShape {
    var circle: Bool
    var inset: CGFloat = 0

    func path(in rect: CGRect) -> Path {
        let r = rect.insetBy(dx: inset, dy: inset)
        return circle ? Circle().path(in: r) : Capsule().path(in: r)
    }

    func inset(by amount: CGFloat) -> ButtonShape { ButtonShape(circle: circle, inset: inset + amount) }
}

extension ButtonStyle where Self == LightButtonStyle {
    static func light(_ color: Color = DottyLight.firefly.color, circle: Bool = false, padded: Bool = true) -> LightButtonStyle {
        LightButtonStyle(light: color, circle: circle, padded: padded)
    }
}

extension ButtonStyle where Self == FrostedButtonStyle {
    static var frosted: FrostedButtonStyle { FrostedButtonStyle() }
    static var frostedCircle: FrostedButtonStyle { FrostedButtonStyle(circle: true) }
}

extension ButtonStyle where Self == GradientButtonStyle {
    static func gradient(_ colors: [Color], vertical: Bool = false) -> GradientButtonStyle {
        GradientButtonStyle(colors: colors, vertical: vertical)
    }
}

extension ButtonStyle where Self == QuietButtonStyle {
    static func quiet(_ color: Color = DottyLight.firefly.color) -> QuietButtonStyle { QuietButtonStyle(light: color) }
}
