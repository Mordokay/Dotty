import SwiftUI

/// One face of a MorphButton: its light and what it shows (text, an SF Symbol, or both).
struct MorphFace: Equatable {
    var light: Color
    var title: String?
    var systemImage: String?
}

/// A light button that turns into its next face when tapped. The content cross-fades, the
/// colour flows to the new light, and the button stretches to its new size like jelly: the
/// width springs past its target and wobbles back while the height squashes and recovers.
struct MorphButton: View {
    let faces: [MorphFace]
    var action: (Int) -> Void = { _ in }

    @State private var index = 0
    @State private var widths: [Int: CGFloat] = [:]
    @State private var stretches = 0

    private static let height: CGFloat = 52
    private static let grow = Animation.spring(response: 0.55, dampingFraction: 0.4)
    private static let shrink = Animation.spring(response: 0.42, dampingFraction: 0.85)

    var body: some View {
        let face = faces[index]
        Button {
            // One animated change, so the row around the button reflows every frame and its
            // neighbours are pushed along. Growing bounces like jelly; shrinking settles gently,
            // so the capsule never dips below its content.
            let next = (index + 1) % faces.count
            let growing = (widths[next] ?? Self.height) > (widths[index] ?? Self.height)
            withAnimation(growing ? Self.grow : Self.shrink) {
                index = next
            }
            stretches += 1
            action(index)
        } label: {
            ZStack {
                // The content swaps with its own quick fade (no bounce, so the old face can never
                // fade back in while the width springs).
                content(face)
                    .id(index)
                    .transition(AnyTransition.opacity.combined(with: .scale(scale: 0.8)).animation(.easeOut(duration: 0.16)))
            }
            .frame(width: widths[index] ?? Self.height, height: Self.height)
        }
        .buttonStyle(.light(face.light, padded: false))
        .keyframeAnimator(initialValue: 1.0, trigger: stretches) { content, height in
            // Squash while it widens, then recover with a little bounce: the volume stays.
            content.scaleEffect(x: 1, y: height)
        } keyframes: { _ in
            KeyframeTrack {
                CubicKeyframe(0.84, duration: 0.12)
                SpringKeyframe(1, duration: 0.6, spring: .init(response: 0.42, dampingRatio: 0.38))
            }
        }
        .background { measurements }
        .accessibilityLabel(face.title ?? face.systemImage ?? "")
    }

    private func content(_ face: MorphFace) -> some View {
        HStack(spacing: Spacing.s) {
            if let symbol = face.systemImage {
                Image(systemName: symbol)
                    .font(.system(size: 20, weight: .semibold, design: .rounded))
            }
            if let title = face.title {
                Text(title)
                    .lineLimit(1)
                    .fixedSize()
            }
        }
    }

    /// Every face laid out invisibly, to know each one's natural width.
    private var measurements: some View {
        ZStack {
            ForEach(faces.indices, id: \.self) { i in
                content(faces[i])
                    .font(.lpHeadline)
                    .fixedSize()
                    .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { width in
                        widths[i] = faces[i].title == nil ? Self.height : width + Spacing.xl * 2
                    }
            }
        }
        .hidden()
        .accessibilityHidden(true)
    }
}
