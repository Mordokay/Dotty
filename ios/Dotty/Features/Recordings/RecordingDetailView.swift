import AVFoundation
import SwiftUI

/// One voice note: play this iPhone's copy, the summary and points, the transcript with Final / Apple tabs
/// (to see how Apple's recognition does with the user's accent), share, rename, delete, play on Dotty.
struct RecordingDetailView: View {
    let companion: String
    let id: String
    let onChanged: () -> Void
    @Environment(DottyLink.self) private var link
    @Environment(\.dismiss) private var dismiss
    @State private var recording: LocalRecording?
    @State private var player: AVAudioPlayer?
    @State private var playing = false
    @State private var tab = "final"
    @State private var renaming = false
    @State private var newTitle = ""
    @State private var confirmDelete = false
    @State private var error: String?

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.l) {
                    PageHeader(title: recording?.title ?? "Recording", subtitle: recording.map(caption))
                        .padding(.top, Spacing.l)
                    if let error { NoticeCard(kind: .error, text: error) }
                    if let r = recording {
                        HStack {
                            Button { toggle(r) } label: { Label(playing ? "Pause" : "Play", systemImage: playing ? "pause.fill" : "play.fill") }
                                .buttonStyle(.light())
                            Spacer()
                            ShareLink(item: RecordingStore.wavURL(r)) { Image(systemName: "square.and.arrow.up") }
                        }
                        if !r.summary.isEmpty || !r.points.isEmpty {
                            GlassCard(title: "Summary") {
                                VStack(alignment: .leading, spacing: Spacing.s) {
                                    Text(r.summary).font(.lpBody).foregroundStyle(Color.ink)
                                    ForEach(r.points, id: \.self) { p in
                                        Text("\u{2022} \(p)").font(.lpBody).foregroundStyle(Color.ink)
                                    }
                                }
                                .padding(Spacing.m)
                            }
                        }
                        GlassCard(title: "Transcript") {
                            VStack(alignment: .leading, spacing: Spacing.s) {
                                Picker("", selection: $tab) {
                                    Text("Final (Gemini)").tag("final")
                                    Text("Apple").tag("apple")
                                }
                                .pickerStyle(.segmented)
                                let text = tab == "final" ? (r.final.isEmpty ? "Not ready yet." : r.final) : (r.draft.isEmpty ? "No draft." : r.draft)
                                Text(text).font(.lpBody).foregroundStyle(Color.ink).textSelection(.enabled)
                                ShareLink(item: text) { Label("Share the text", systemImage: "text.quote") }.font(.lpCaption)
                            }
                            .padding(Spacing.m)
                        }
                        HStack {
                            Button("Rename") { newTitle = r.title; renaming = true }.buttonStyle(.quiet())
                            Button("Play on Dotty") { Task { _ = try? await link.send("companion.recording.play", ["companion": companion, "name": id]) } }
                                .buttonStyle(.quiet())
                                .needsDotty(link)
                            Spacer()
                            Button("Delete", role: .destructive) { confirmDelete = true }.buttonStyle(.quiet(DottyLight.ember.color))
                        }
                    } else {
                        NoticeCard(kind: .info, text: "This recording isn't on this iPhone yet: it syncs the next time Dotty and the app meet.")
                    }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
        }
        .navigationTitle("")
        .onAppear { recording = RecordingStore.find(companion: companion, id: id) }
        .onDisappear { player?.stop() }
        .alert("Rename", isPresented: $renaming) {
            TextField("Title", text: $newTitle)
            Button("Save") { Task { await rename() } }
            Button("Cancel", role: .cancel) {}
        }
        .confirmationDialog("Delete this recording from Dotty and this iPhone?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) { Task { await delete() } }
        }
    }

    private func caption(_ r: LocalRecording) -> String {
        r.created.formatted(.dateTime.weekday(.wide).day().month().hour().minute()) +
            " · " + String(format: "%d:%02d", Int(r.duration) / 60, Int(r.duration) % 60)
    }

    private func toggle(_ r: LocalRecording) {
        if playing {
            player?.pause()
            playing = false
            return
        }
        if player == nil {
            try? AVAudioSession.sharedInstance().setCategory(.playback)
            player = try? AVAudioPlayer(contentsOf: RecordingStore.wavURL(r))
        }
        playing = player?.play() ?? false
    }

    private func rename() async {
        let title = newTitle.trimmingCharacters(in: .whitespacesAndNewlines)
        guard var r = recording, !title.isEmpty else { return }
        r.title = String(title.prefix(80))
        RecordingStore.save(r)
        recording = r
        _ = try? await link.send("companion.recording.rename", ["companion": companion, "name": id, "title": r.title])
        onChanged()
    }

    private func delete() async {
        do {
            _ = try await link.send("companion.recording.delete", ["companion": companion, "name": id])
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription; return }
        }
        if let r = recording { RecordingStore.delete(r) }
        onChanged()
        dismiss()
    }
}
