import SwiftUI

// The Dotty design system's tokens, mirroring Design/System/project/tokens.json one to one.

extension Color {
    init(hex: UInt32, opacity: Double = 1) {
        self.init(.sRGB,
                  red: Double((hex >> 16) & 0xFF) / 255,
                  green: Double((hex >> 8) & 0xFF) / 255,
                  blue: Double(hex & 0xFF) / 255,
                  opacity: opacity)
    }

    /// The base of every screen: the dark that light is seen against.
    static let void = Color(hex: 0x07090F)
    /// Solid dark surfaces where glass would be too busy.
    static let voidRaised = Color(hex: 0x111521)
    /// Text and icons, warm white.
    static let ink = Color(hex: 0xF5F1E6)
    /// Secondary text and metadata.
    static let inkMuted = Color(hex: 0xA7ACBC)
    /// Placeholders and disabled text only.
    static let inkFaint = Color(hex: 0x7E8498)
    /// Text and icons on a light fill.
    static let onLight = Color(hex: 0x10130A)
    /// The fill of glass surfaces, over a backdrop blur.
    static let glass = Color.white.opacity(0.07)
    /// Pressed or selected glass, and off tracks.
    static let glassStrong = Color.white.opacity(0.12)
    /// The lit rim along the top edge of glass.
    static let glassEdge = Color.white.opacity(0.18)
    /// Darkens glass so text stays readable over any colour.
    static let glassVeil = Color(hex: 0x07090F, opacity: 0.45)
}

/// A colour a bracelet can show, and the app's light in that colour.
enum DottyLight: String, CaseIterable, Identifiable, Sendable {
    case firefly, leaf, lagoon, dusk, bloom, ember, amber

    var id: String { rawValue }
    var name: String { rawValue.capitalized }

    var hex: UInt32 {
        switch self {
        case .firefly: 0xE9F25A
        case .leaf: 0x6EE07A
        case .lagoon: 0x3FD4E6
        case .dusk: 0x8C86FF
        case .bloom: 0xFF70C6
        case .ember: 0xFF7A45
        case .amber: 0xFFB547
        }
    }

    var color: Color { Color(hex: hex) }

    /// 0...1 RGB, for shaders and the bracelet.
    var rgb: SIMD3<Float> {
        SIMD3(Float((hex >> 16) & 0xFF), Float((hex >> 8) & 0xFF), Float(hex & 0xFF)) / 255
    }
}

enum Spacing {
    static let xs: CGFloat = 4
    static let s: CGFloat = 8
    static let m: CGFloat = 12
    static let l: CGFloat = 16
    static let xl: CGFloat = 24
    static let xxl: CGFloat = 32
    static let xxxl: CGFloat = 48
}

enum Radius {
    static let soft: CGFloat = 16
    static let card: CGFloat = 28
    static let sheet: CGFloat = 40
}

/// Parallax depths: scrolling moves a layer by its depth; tilting moves it by (1 - depth) × tiltReach.
enum Depth {
    static let far: CGFloat = 0.15
    static let back: CGFloat = 0.5
    static let ui: CGFloat = 1
    static let near: CGFloat = 1.15
    static let tiltReach: CGFloat = 46
}

enum Durations {
    static let tap = 0.16
    static let settle = 0.42
    /// One light breath, the same period the bracelet breathes at.
    static let pulse = 2.4
    static let float = 4.8
    static let drift = 14.0
}

extension Animation {
    /// Pulses, floats and drifts: symmetric, so loops have no seam.
    static let breathe = Animation.timingCurve(0.45, 0, 0.55, 1, duration: Durations.pulse)
    /// Things that land: a small overshoot, like a spring.
    static let settle = Animation.spring(response: Durations.settle, dampingFraction: 0.62)
}

extension Font {
    /// Quicksand Bold, for titles and the wordmark (bundled, registered at launch).
    static func display(_ size: CGFloat) -> Font { .custom("Quicksand", size: size).weight(.bold) }

    static let lpDisplayXL = display(40)
    static let lpDisplayL = display(32)
    static let lpTitle = display(24)
    static let lpHeadline = Font.system(.headline, design: .rounded)
    static let lpBody = Font.system(.body, design: .rounded)
    static let lpCallout = Font.system(.callout, design: .rounded).weight(.medium)
    static let lpCaption = Font.system(.caption, design: .rounded).weight(.medium)
    static let lpLabel = Font.system(.caption2, design: .rounded).weight(.bold)
}
