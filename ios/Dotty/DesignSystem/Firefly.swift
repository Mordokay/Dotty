import SwiftUI

/// The only loading indicator: the firefly floating, its tail glowing and breathing in `light`.
struct FireflyLoader: View {
    var size: CGFloat = 120
    var light: Color = DottyLight.firefly.color
    var label = "Loading"

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.motionActive) private var active

    private var still: Bool { reduceMotion || !active }

    var body: some View {
        ZStack {
            halo
                .offset(y: size * 0.2)
            TintedFirefly(color: light, size: size)
                .phaseAnimator(still ? [0.0] : [0.0, 1.0]) { content, phase in
                    content.offset(y: -size * 0.06 * phase)
                } animation: { _ in .timingCurve(0.45, 0, 0.55, 1, duration: Durations.float / 2) }
        }
        .frame(width: size, height: size)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(label)
    }

    private var halo: some View {
        Circle()
            .fill(RadialGradient(colors: [.white, light.mix(with: .white, by: 0.25), light.opacity(0.45), .clear],
                                 center: .center, startRadius: 0, endRadius: size * 0.3))
            .frame(width: size * 0.6, height: size * 0.6)
            .blendMode(.plusLighter)
            .phaseAnimator(still ? [0.7] : [0.0, 1.0]) { content, phase in
                content.scaleEffect(0.8 + 0.5 * phase).opacity(0.3 + 0.7 * phase)
            } animation: { _ in .breathe }
    }
}

/// The firefly mark with the lowercase `dotty` wordmark.
struct DottyLogo: View {
    var size: CGFloat = 96
    var horizontal = false

    var body: some View {
        let layout = horizontal ? AnyLayout(HStackLayout(spacing: Spacing.m)) : AnyLayout(VStackLayout(spacing: Spacing.xs))
        layout {
            Image("FireflyMark")
                .resizable()
                .scaledToFit()
                .frame(width: size, height: size)
            Text("dotty")
                .font(.display(size * (horizontal ? 0.5 : 0.36)))
                .foregroundStyle(Color.ink)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Dotty")
    }
}
