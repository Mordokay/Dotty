import SwiftUI

/// Our map pin: a glowing teardrop in a firefly colour carrying the place's icon (directions plan, Task 7).
struct PlacePin: View {
    let icon: PlaceIcon
    var size: CGFloat = 36
    var selected = false

    var body: some View {
        let light = icon.light.color
        VStack(spacing: -size * 0.18) {
            ZStack {
                Circle().fill(light).shadow(color: light.opacity(0.7), radius: selected ? 10 : 5)
                Circle().strokeBorder(Color.black.opacity(0.55), lineWidth: 1.5)
                Image(systemName: icon.symbol)
                    .font(.system(size: size * 0.42, weight: .bold))
                    .foregroundStyle(Color.black.opacity(0.78))
            }
            .frame(width: size, height: size)
            Triangle()
                .fill(light)
                .overlay(Triangle().stroke(Color.black.opacity(0.55), lineWidth: 1.5))
                .frame(width: size * 0.42, height: size * 0.36)
        }
        .scaleEffect(selected ? 1.15 : 1, anchor: .bottom)
        .animation(.settle, value: selected)
    }
}

/// The pin's point, down at the place.
nonisolated private struct Triangle: Shape {
    func path(in r: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: r.minX, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX, y: r.minY))
        p.addLine(to: CGPoint(x: r.midX, y: r.maxY))
        p.closeSubpath()
        return p
    }
}

extension PlaceIcon {
    /// Each icon its own light from the design system.
    var light: DottyLight {
        switch self {
        case .home: .firefly
        case .work: .lagoon
        case .heart: .bloom
        case .star: .amber
        case .cup: .ember
        case .dumbbell: .leaf
        case .cart: .dusk
        case .pin: .firefly
        }
    }
}
