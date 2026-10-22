import AVFoundation
import SwiftUI

/// The companion's voice (Gemini's prebuilt voices): a recorded sample of each, bundled with the
/// app (Resources/Voices, made by tools/voice_samples.py), to hear before picking one. Starred
/// voices are the ones Dotty's own settings screen steps through (with their samples, built into
/// the firmware by tools/build_voice_pack.py).
struct VoicesView: View {
    @State private var selected: String
    @State private var favourites: [String]
    let onPick: (String) -> Void
    let onFavourites: ([String]) -> Void

    @State private var playing: String?
    @State private var player: AVAudioPlayer?
    @State private var finished = Finished()

    init(selected: String, favourites: [String], onPick: @escaping (String) -> Void,
         onFavourites: @escaping ([String]) -> Void) {
        _selected = State(initialValue: selected)
        _favourites = State(initialValue: favourites)
        self.onPick = onPick
        self.onFavourites = onFavourites
    }

    /// Name and Google's one-word style, the user's favourites first (as the firmware's voices.h).
    static let voices: [(name: String, style: String)] = [
        ("Zephyr", "Bright"), ("Achernar", "Soft"), ("Puck", "Upbeat"), ("Charon", "Informative"), ("Kore", "Firm"),
        ("Fenrir", "Excitable"), ("Leda", "Youthful"), ("Orus", "Firm"), ("Aoede", "Breezy"),
        ("Callirrhoe", "Easy-going"), ("Autonoe", "Bright"), ("Enceladus", "Breathy"), ("Iapetus", "Clear"),
        ("Umbriel", "Easy-going"), ("Algieba", "Smooth"), ("Despina", "Smooth"), ("Erinome", "Clear"),
        ("Algenib", "Gravelly"), ("Rasalgethi", "Informative"), ("Laomedeia", "Upbeat"),
        ("Alnilam", "Firm"), ("Schedar", "Even"), ("Gacrux", "Mature"), ("Pulcherrima", "Forward"),
        ("Achird", "Friendly"), ("Zubenelgenubi", "Casual"), ("Vindemiatrix", "Gentle"), ("Sadachbia", "Lively"),
        ("Sadaltager", "Knowledgeable"), ("Sulafat", "Warm"),
    ]

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Voices",
                               subtitle: "Tap one to choose it, ▶ to listen. Star the ones you like: Dotty's settings step through those.")
                        .padding(.top, Spacing.l)
                    if !favourites.isEmpty {
                        GlassCard(title: "On Dotty") {
                            ForEach(favourites, id: \.self) { name in
                                if let voice = Self.voices.first(where: { $0.name == name }) { row(voice) }
                            }
                        }
                    }
                    GlassCard(title: "All \(Self.voices.count) voices") {
                        ForEach(Self.voices.filter { !favourites.contains($0.name) }, id: \.name) { row($0) }
                    }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
        }
        .navigationTitle("")
        .onDisappear { stop() }
    }

    private func row(_ voice: (name: String, style: String)) -> some View {
        ChoiceRow(title: voice.name, subtitle: voice.style, chosen: voice.name == selected, action: {
            selected = voice.name
            onPick(voice.name)
        }) {
            Button { toggle(voice.name) } label: {
                Image(systemName: playing == voice.name ? "stop.circle.fill" : "play.circle")
                    .font(.system(size: 22, weight: .semibold, design: .rounded))
                    .foregroundStyle(Color.ink)
            }
            .buttonStyle(.plain)
            .disabled(url(voice.name) == nil)
            .accessibilityLabel(playing == voice.name ? "Stop" : "Listen to \(voice.name)")
            Button { star(voice.name) } label: {
                Image(systemName: favourites.contains(voice.name) ? "star.fill" : "star")
                    .font(.system(size: 20, weight: .semibold, design: .rounded))
                    .foregroundStyle(favourites.contains(voice.name) ? DottyLight.firefly.color : Color.inkMuted)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(favourites.contains(voice.name) ? "Unstar" : "Star")
        }
    }

    private func star(_ name: String) {
        if let i = favourites.firstIndex(of: name) {
            guard favourites.count > 1 else { return }  // Dotty needs at least one
            favourites.remove(at: i)
        } else {
            favourites.append(name)
        }
        onFavourites(favourites)
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
