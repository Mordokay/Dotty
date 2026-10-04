import SwiftUI

/// How a glass surface's rim looks. Both are static, drawn once (no per-frame lighting).
enum RimLighting: Equatable {
    /// A thin lit edge along the top, as glass catches light; tinted by the panel's light if it has one.
    case glass
    /// A fixed colour painted along the rim, brightest along the top.
    case painted(Color)
}

/// Frosted glass: blurs the light behind it so colour bleeds through, darkened by a veil that
/// keeps text readable. `light` is the panel's own light (tinting it from inside); `rim` gives
/// the plain glass edge or a painted rim colour.
struct GlassBackground: View {
    var cornerRadius: CGFloat
    var light: Color?
    var rim: RimLighting = .glass
    /// A real backdrop blur. It re-blurs whatever moves behind it every frame, so keep it for
    /// hero panels; the default is a dark tinted fill that costs nothing.
    var frosted = false

    private var shape: RoundedRectangle { RoundedRectangle(cornerRadius: cornerRadius, style: .continuous) }

    private var rimColor: Color? {
        if case .painted(let color) = rim { return color }
        return light
    }

    var body: some View {
        ZStack {
            if frosted {
                shape.fill(.ultraThinMaterial)
                shape.fill(Color.glassVeil)
            } else {
                shape.fill(Color(hex: 0x111522, opacity: 0.86))
            }
            shape.fill(LinearGradient(colors: [.glassStrong, .glass, .clear], startPoint: .top, endPoint: .bottom))
            if let tint = rimColor {
                shape.fill(RadialGradient(colors: [tint.opacity(0.26), .clear], center: .topLeading, startRadius: 0, endRadius: 280))
                    .blendMode(.screen)
            }
        }
        .overlay {
            if let color = rimColor {
                PaintedRim(shape: shape, color: color)
            } else {
                shape.strokeBorder(LinearGradient(colors: [.glassEdge, .white.opacity(0.03)], startPoint: .top, endPoint: .center), lineWidth: 1)
            }
        }
    }
}

/// A rim painted in one colour, brightest along the top as if lit from above.
private struct PaintedRim: View {
    let shape: RoundedRectangle
    let color: Color

    var body: some View {
        let gradient = LinearGradient(stops: [
            .init(color: color.mix(with: .white, by: 0.25), location: 0),
            .init(color: color.opacity(0.55), location: 0.35),
            .init(color: color.opacity(0.08), location: 1),
        ], startPoint: .top, endPoint: .bottom)
        // Two plain strokes (a wide faint one under a thin bright one) read as a glowing edge,
        // with no blur and nothing recomputed.
        ZStack {
            shape.stroke(gradient, lineWidth: 7).opacity(0.18)
            shape.stroke(gradient, lineWidth: 3.5).opacity(0.3)
            shape.strokeBorder(gradient, lineWidth: 1.4)
        }
        .allowsHitTesting(false)
    }
}

extension View {
    /// Puts this view on frosted glass, with a glass edge or a painted rim.
    func glassSurface(light: Color? = nil, cornerRadius: CGFloat = Radius.card, rim: RimLighting = .glass,
                      frosted: Bool = false) -> some View {
        background { GlassBackground(cornerRadius: cornerRadius, light: light, rim: rim, frosted: frosted) }
    }
}

/// A group of content on glass, with an optional Quicksand title.
struct GlassCard<Content: View>: View {
    var title: String?
    var light: Color?
    var rim: RimLighting = .glass
    var frosted = false
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if let title {
                Text(title)
                    .font(.lpTitle)
                    .foregroundStyle(Color.ink)
                    .padding(.horizontal, Spacing.m)
                    .padding(.top, Spacing.s)
                    .padding(.bottom, Spacing.xs)
            }
            content
        }
        .padding(Spacing.s)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassSurface(light: light, rim: rim, frosted: frosted)
    }
}
