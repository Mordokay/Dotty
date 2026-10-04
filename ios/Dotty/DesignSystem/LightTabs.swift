import SwiftUI

/// A segmented control on glass: the chosen segment glows. For switching between a few pages
/// of one screen (e.g. Player · Library · Playlists).
struct LightTabs<Tab: Hashable>: View {
    let tabs: [Tab]
    @Binding var selection: Tab
    var light: Color = DottyLight.firefly.color
    let label: (Tab) -> String

    @Namespace private var glow

    var body: some View {
        HStack(spacing: Spacing.xs) {
            ForEach(tabs, id: \.self) { tab in
                let chosen = tab == selection
                Button {
                    withAnimation(.settle) { selection = tab }
                } label: {
                    Text(label(tab))
                        .font(.lpCallout.weight(.semibold))
                        .lineLimit(1)
                        .foregroundStyle(chosen ? Color.onLight : Color.inkMuted)
                        .frame(maxWidth: .infinity, minHeight: 36)
                        .background {
                            if chosen {
                                Capsule()
                                    .fill(light)
                                    .shadow(color: light.opacity(0.6), radius: 10)
                                    .matchedGeometryEffect(id: "glow", in: glow)
                            }
                        }
                        .contentShape(Capsule())
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(chosen ? .isSelected : [])
            }
        }
        .padding(Spacing.xs)
        .glassSurface(cornerRadius: 24)
        .sensoryFeedback(Haptic.select, trigger: selection)
    }
}
