import SwiftUI
import UIKit

/// How much the app spends on looks. Auto drops to Low under Low Power Mode or when the phone
/// runs hot; High and Low are fixed choices (the showcase lets you switch).
enum RenderQuality: String, CaseIterable, Identifiable, Sendable {
    case auto, high, low

    var id: String { rawValue }
    var name: String { rawValue.capitalized }

    /// What the effects should actually do right now.
    var isLow: Bool {
        switch self {
        case .high: false
        case .low: true
        case .auto:
            ProcessInfo.processInfo.isLowPowerModeEnabled
                || ProcessInfo.processInfo.thermalState == .serious
                || ProcessInfo.processInfo.thermalState == .critical
        }
    }
}

extension EnvironmentValues {
    /// The app's rendering quality.
    @Entry var renderQuality: RenderQuality = .auto
    /// False while a view is scrolled off screen, so its animations can pause.
    @Entry var motionActive: Bool = true
    /// Whether sliders throw sparks when dragged fast.
    @Entry var sliderSparks: Bool = false
}

extension View {
    /// Pauses this section's animations while it is scrolled out of view.
    func pausesOffscreen() -> some View {
        modifier(PausesOffscreen())
    }
}

private struct PausesOffscreen: ViewModifier {
    @State private var visible = true

    func body(content: Content) -> some View {
        content
            .environment(\.motionActive, visible)
            .onScrollVisibilityChange(threshold: 0.01) { visible = $0 }
    }
}

/// A soft glow behind something lit, faked with gradients (no light, no shader, no blur).
/// With `wobble`, two slightly oval layers turn slowly against each other, so the glow's edge
/// breathes unevenly; only their rotation animates, which is cheap. Place it with negative
/// padding (or a larger frame) so it can spread beyond what it lights.
struct GlowView: View {
    var color: Color
    /// 0...1: how strong the glow is.
    var intensity: Double = 0.4
    /// How far the bright part reaches, relative to the frame (0...1).
    var core: Double = 0.55
    var wobble = true

    @Environment(\.renderQuality) private var quality
    @Environment(\.motionActive) private var active
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var turning = false

    var body: some View {
        let moving = wobble && active && !quality.isLow && !reduceMotion
        ZStack {
            glow(strength: moving ? 0.6 : 1)
            if moving {
                glow(strength: 0.25)
                    .scaleEffect(x: 1.1, y: 0.9)
                    .rotationEffect(.degrees(turning ? 360 : 0))
                glow(strength: 0.2)
                    .scaleEffect(x: 0.92, y: 1.08)
                    .rotationEffect(.degrees(turning ? -360 : 0))
            }
        }
        .animation(moving ? .linear(duration: 16).repeatForever(autoreverses: false) : nil, value: turning)
        .onAppear { turning = moving }
        .onChange(of: moving) { _, now in turning = now }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private func glow(strength: Double) -> some View {
        Ellipse().fill(EllipticalGradient(stops: [
            .init(color: color.opacity(intensity * strength), location: 0),
            .init(color: color.opacity(intensity * strength * 0.45), location: core * 0.55),
            .init(color: color.opacity(0), location: 1),
        ], center: .center, startRadiusFraction: 0, endRadiusFraction: 0.5))
    }
}

/// A glow that follows a shape's outline, faked with layers (no light, no shader, no blur):
/// copies of the shape, each a little larger and faint, stacked so they pile up brightest at
/// the shape's edge and thin out away from it.
struct ShapeGlow<S: Shape>: View {
    var shape: S
    var color: Color
    /// How far the glow reaches beyond the shape, in points.
    var reach: CGFloat = 14
    /// 0...1: how strong it is at the shape's edge.
    var intensity: Double = 0.45

    private static var layers: Int { 16 }

    var body: some View {
        let each = 1 - pow(1 - intensity, 1 / Double(Self.layers))
        ZStack {
            ForEach(1...Self.layers, id: \.self) { layer in
                shape
                    .fill(color.opacity(each))
                    .padding(-reach * CGFloat(layer) / CGFloat(Self.layers))
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

extension Color {
    /// 0...1 RGB.
    var rgb: SIMD3<Double> {
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        UIColor(self).getRed(&r, green: &g, blue: &b, alpha: &a)
        return SIMD3(Double(r), Double(g), Double(b))
    }
}
