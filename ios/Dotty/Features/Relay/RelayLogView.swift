import SwiftUI

/// The relay's recent activity (what went through this iPhone, and whether the app was in the
/// background): for checking a chat through the phone.
struct RelayLogView: View {
    private let relay = RelayService.shared

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.s) {
                    PageHeader(title: "Relay log", subtitle: "Chats with Dotty going through this iPhone.")
                        .padding(.top, Spacing.l)
                    Button {
                        Task { relay.log(await SpeechProbe.run { relay.log($0) }) }
                    } label: {
                        Label("Background speech test", systemImage: "waveform")
                    }
                    .buttonStyle(.quiet())
                    Text("Tap it, lock the iPhone at once, unlock after 30 s: the lines below say whether Apple's speech recognition kept going in the background.")
                        .font(.lpCaption).foregroundStyle(Color.inkFaint)
                    if relay.lines.isEmpty {
                        Text("Nothing yet. Hold BOOT on Dotty with this iPhone connected.").font(.lpCaption).foregroundStyle(Color.inkFaint)
                    }
                    ForEach(Array(relay.lines.enumerated()), id: \.offset) { _, line in
                        Text(line).font(.system(.footnote, design: .monospaced)).foregroundStyle(Color.ink)
                    }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
        }
        .navigationTitle("")
    }
}
