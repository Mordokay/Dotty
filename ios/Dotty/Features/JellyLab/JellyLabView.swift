import SwiftUI

/// A test bench for jelly glass: Liquid Glass shaped by soft-body physics, lit from inside.
struct JellyLabView: View {
    @State var light: DottyLight
    @State private var presses = 0

    @Environment(\.dismiss) private var dismiss

    private var specs: [JellySpec] {
        [
            JellySpec(id: "tap", light: light.color, size: CGSize(width: 150, height: 150), center: CGPoint(x: 180, y: 85),
                      label: AnyView(Image(systemName: "hand.tap.fill").font(.system(size: 44, weight: .semibold, design: .rounded))),
                      action: { presses += 1 }),
            JellySpec(id: "send", light: light.color, size: CGSize(width: 250, height: 74), center: CGPoint(x: 180, y: 222),
                      label: AnyView(Text("Send touch").font(.display(24))),
                      action: { presses += 1 }),
            JellySpec(id: "lagoon", light: DottyLight.lagoon.color, size: CGSize(width: 120, height: 60), center: CGPoint(x: 108, y: 320),
                      label: AnyView(Text("Lagoon").font(.lpHeadline)),
                      action: { presses += 1 }),
            JellySpec(id: "bloom", light: DottyLight.bloom.color, size: CGSize(width: 120, height: 60), center: CGPoint(x: 252, y: 320),
                      label: AnyView(Text("Bloom").font(.lpHeadline)),
                      action: { presses += 1 }),
        ]
    }

    var body: some View {
        LightField(lights: [light.color, DottyLight.lagoon.color, DottyLight.bloom.color], seed: 11) {
            VStack(spacing: Spacing.l) {
                HStack {
                    Button { dismiss() } label: { Image(systemName: "chevron.left") }
                        .buttonStyle(.frostedCircle)
                        .accessibilityLabel("Back")
                    Spacer()
                }
                VStack(alignment: .leading, spacing: Spacing.s) {
                    Text("Jelly lab")
                        .font(.lpDisplayL)
                        .foregroundStyle(Color.ink)
                    Text("Press and hold to pour light in. Poke to wobble. Pull an edge to stretch it, or push one button into another.")
                        .font(.lpBody)
                        .foregroundStyle(Color.inkMuted)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .frame(maxWidth: .infinity, alignment: .leading)

                Spacer()

                // All four buttons live in one physics world: push one into another and it shoves
                // its neighbour aside, their glass melting together where they touch.
                JellyCluster(size: CGSize(width: 360, height: 360), draggable: true, specs: specs)

                Spacer()

                Text("Pressed \(presses) times")
                    .font(.lpCaption)
                    .foregroundStyle(Color.inkMuted)
                    .contentTransition(.numericText())
                    .animation(.settle, value: presses)
                LightColorPicker(selection: $light)
            }
            .padding(.horizontal, Spacing.l)
            .padding(.top, Spacing.m)
            .padding(.bottom, Spacing.s)
        }
        .toolbar(.hidden, for: .navigationBar)
        .sensoryFeedback(Haptic.confirm, trigger: presses)
    }
}

#Preview {
    JellyLabView(light: .firefly)
}
