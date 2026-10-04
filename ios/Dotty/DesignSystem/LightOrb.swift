import SwiftUI

/// A single light source: a hot white core, the light colour, and a halo fading into the dark.
/// It stands for the bracelet and for colours you can pick. `pulse` breathes it on the bracelet's
/// period.
struct LightOrb: View {
    var color: Color = DottyLight.firefly.color
    var size: CGFloat = 64
    var pulse = false

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.motionActive) private var active

    var body: some View {
        ZStack {
            // The halo is a shader glow, much cheaper to animate than blurred shadows.
            GlowView(color: color, intensity: 0.5, core: 0.42, wobble: pulse)
                .frame(width: size * 2.4, height: size * 2.4)
                .modifier(PulseModifier(active: pulse && !reduceMotion && active))
            orb
        }
        .frame(width: size, height: size)
    }

    private var orb: some View {
        Circle()
            .fill(RadialGradient(
                stops: [
                    .init(color: .white, location: 0),
                    .init(color: color.mix(with: .white, by: 0.45), location: 0.18),
                    .init(color: color, location: 0.5),
                    .init(color: color.mix(with: .black, by: 0.45), location: 1),
                ],
                center: UnitPoint(x: 0.38, y: 0.34), startRadius: 0, endRadius: size * 0.62))
            .frame(width: size, height: size)
    }
}

/// Breathes a view on the bracelet's period: scale and opacity only, which are cheap to animate.
private struct PulseModifier: ViewModifier {
    let active: Bool

    func body(content: Content) -> some View {
        if active {
            content.phaseAnimator([0.0, 1.0]) { view, phase in
                view.scaleEffect(0.85 + 0.25 * phase).opacity(0.55 + 0.45 * phase)
            } animation: { _ in .breathe }
        } else {
            content
        }
    }
}

#Preview {
    HStack(spacing: 24) {
        LightOrb(color: DottyLight.firefly.color, size: 96, pulse: true)
        LightOrb(color: DottyLight.lagoon.color)
        LightOrb(color: DottyLight.bloom.color, size: 40)
    }
    .padding(60)
    .background(Color.void)
}
