import AVFoundation
import SwiftUI

/// The AI companion's voices (Gemini's prebuilt voices, which Dotty speaks with): a recorded
/// sample of each, bundled with the app (Resources/Voices, made by tools/voice_samples.py), to
/// hear them before picking one for a role.
struct VoicesView: View {
    @State private var playing: String?
    @State private var player: AVAudioPlayer?
    @State private var finished = Finished()

    /// Name and Google's one-word style, in tools/voice_samples.py's order.
    static let voices: [(name: String, style: String)] = [
        ("Zephyr", "Bright"), ("Puck", "Upbeat"), ("Charon", "Informative"), ("Kore", "Firm"),
        ("Fenrir", "Excitable"), ("Leda", "Youthful"), ("Orus", "Firm"), ("Aoede", "Breezy"),
        ("Callirrhoe", "Easy-going"), ("Autonoe", "Bright"), ("Enceladus", "Breathy"), ("Iapetus", "Clear"),
        ("Umbriel", "Easy-going"), ("Algieba", "Smooth"), ("Despina", "Smooth"), ("Erinome", "Clear"),
        ("Algenib", "Gravelly"), ("Rasalgethi", "Informative"), ("Laomedeia", "Upbeat"), ("Achernar", "Soft"),
        ("Alnilam", "Firm"), ("Schedar", "Even"), ("Gacrux", "Mature"), ("Pulcherrima", "Forward"),
        ("Achird", "Friendly"), ("Zubenelgenubi", "Casual"), ("Vindemiatrix", "Gentle"), ("Sadachbia", "Lively"),
        ("Sadaltager", "Knowledgeable"), ("Sulafat", "Warm"),
    ]

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Voices",
                               subtitle: "How your companion can sound. Each one says the same lines; tap to listen.")
                        .padding(.top, Spacing.l)
                    GlassCard(title: "\(Self.voices.count) voices") {
                        ForEach(Self.voices, id: \.name) { voice in
                            let available = url(voice.name) != nil
                            LightRow(title: voice.name, subtitle: available ? voice.style : "\(voice.style) · no sample yet",
                                     systemImage: playing == voice.name ? "stop.circle.fill" : "play.circle",
                                     action: { toggle(voice.name) })
                                .disabled(!available)
                                .opacity(available ? 1 : 0.45)
                        }
                    }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
        }
        .navigationTitle("")
        .onDisappear { stop() }
    }

    private func url(_ name: String) -> URL? {
        Bundle.main.url(forResource: name, withExtension: "m4a")
    }

    private func toggle(_ name: String) {
        if playing == name { return stop() }
        stop()
        guard let url = url(name) else { return }
        do {
            try AVAudioSession.sharedInstance().setCategory(.playback)
            try AVAudioSession.sharedInstance().setActive(true)
            let player = try AVAudioPlayer(contentsOf: url)
            finished.onFinish = { playing = nil }
            player.delegate = finished
            player.play()
            self.player = player
            playing = name
        } catch {
            playing = nil
        }
    }

    private func stop() {
        player?.stop()
        player = nil
        playing = nil
    }
}

private final class Finished: NSObject, AVAudioPlayerDelegate {
    var onFinish: (() -> Void)?
    nonisolated func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        Task { @MainActor in self.onFinish?() }
    }
}
