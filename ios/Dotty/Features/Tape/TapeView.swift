import SwiftUI

/// The Tape Recorder cartridge's screen: what the deck on Dotty is doing, and every
/// recording: listen on the iPhone, share or save it, play it on Dotty, rename, delete.
struct TapeView: View {
    @Environment(DottyLink.self) private var link
    @State private var model: TapeModel?

    var body: some View {
        Group {
            if let model {
                TapeContent(model: model)
            } else {
                LightField { Color.clear }
            }
        }
        .onAppear { if model == nil { model = TapeModel(link: link) } }
    }
}

private struct TapeContent: View {
    @Bindable var model: TapeModel
    @Environment(DottyLink.self) private var link
    @State private var renaming: TapeModel.Recording?
    @State private var newTitle = ""
    @State private var deleting: TapeModel.Recording?

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Tape Recorder", subtitle: "Hold BOOT on Dotty to record, let go to pause.")
                        .padding(.top, Spacing.l)
                    // Bluetooth pauses on purpose while a recording comes over Wi-Fi.
                    if model.fetching.isEmpty { NotConnectedNotice() }
                    if let error = model.error { NoticeCard(kind: .error, text: error) }
                    if let stage = model.fetchStage {
                        HStack(spacing: Spacing.m) {
                            ProgressView().tint(Color.ink)
                            Text(stage).font(.lpCallout).foregroundStyle(Color.ink)
                        }
                        .padding(Spacing.l)
                        .glassSurface()
                    }
                    deckCard
                    recordingsCard
                        .disabled(link.connection != .connected && model.fetching.isEmpty)
                        .opacity(link.connection == .connected || !model.fetching.isEmpty ? 1 : 0.45)
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await model.load() }
        }
        .navigationTitle("")
        .task { await model.load() }
        .onChange(of: link.connection) { _, state in
            if state == .connected { Task { await model.reconnected() } }
        }
        .onChange(of: link.eventCount) { _, _ in
            if let event = link.lastEvent { model.handle(event) }
        }
        .alert("Rename recording", isPresented: .init(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            TextField("Name", text: $newTitle)
            Button("Rename") {
                let title = newTitle.trimmingCharacters(in: .whitespaces)
                if let recording = renaming, !title.isEmpty { Task { _ = await model.rename(recording, to: title) } }
                renaming = nil
            }
            Button("Cancel", role: .cancel) { renaming = nil }
        }
        .confirmationDialog("Delete this recording from Dotty?", isPresented: .init(get: { deleting != nil },
                                                                                  set: { if !$0 { deleting = nil } }),
                            titleVisibility: .visible, presenting: deleting) { recording in
            Button("Delete \(recording.title)", role: .destructive) { Task { await model.delete(recording) } }
        } message: { _ in
            Text("It's removed from the SD card. A copy you saved or shared stays.")
        }
    }

    // MARK: - Deck

    @ViewBuilder private var deckCard: some View {
        let deck = model.deck
        if deck.state != "idle" {
            GlassCard(light: deck.state == "recording" ? DottyLight.ember.color : nil) {
                LightRow(title: deck.state == "recording" ? "Recording" : "Paused",
                         subtitle: deck.state == "recording" ? "Let go of BOOT to pause"
                                                             : "Hold BOOT to go on, or tap Save on Dotty",
                         systemImage: deck.state == "recording" ? "record.circle" : "pause.circle") {
                    Text(Self.time(Double(deck.elapsed))).font(.lpCallout.monospacedDigit())
                }
            }
        } else if let playing = deck.playing, let recording = model.recordings.first(where: { $0.name == playing }) {
            GlassCard {
                LightRow(title: deck.paused ? "Paused on Dotty" : "Playing on Dotty", subtitle: recording.title,
                         systemImage: "hifispeaker") {
                    Button("Stop") { Task { await model.stopOnDotty() } }.buttonStyle(.quiet())
                }
            }
        }
    }

    // MARK: - Recordings

    private var recordingsCard: some View {
        GlassCard {
            Text(model.recordings.count == 1 ? "1 recording" : "\(model.recordings.count) recordings")
                .font(.lpTitle).foregroundStyle(Color.ink)
                .padding(Spacing.m)
            if model.recordings.isEmpty {
                Text(model.loaded ? "Nothing yet. Hold BOOT on Dotty to record." : "Reading Dotty's tapes…")
                    .font(.lpCallout).foregroundStyle(Color.inkMuted)
                    .padding([.horizontal, .bottom], Spacing.m)
            }
            ForEach(model.recordings) { recording in
                row(recording)
            }
        }
    }

    private func row(_ recording: TapeModel.Recording) -> some View {
        let listening = model.listening == recording.name && model.isListening
        let fetching = model.fetching.contains(recording.name)
        return HStack(spacing: Spacing.m) {
            Button {
                Task { await model.listen(to: recording) }
            } label: {
                ZStack {
                    Circle().fill(Color.glassStrong)
                    if fetching {
                        ProgressView().controlSize(.small).tint(Color.ink)
                    } else {
                        Image(systemName: listening ? "pause.fill" : "play.fill")
                            .font(.system(size: 15, weight: .bold))
                            .foregroundStyle(Color.ink)
                    }
                }
                .frame(width: 40, height: 40)
            }
            .buttonStyle(.plain)
            .disabled(!model.fetching.isEmpty && !fetching)
            .accessibilityLabel(listening ? "Pause" : "Listen on iPhone")
            VStack(alignment: .leading, spacing: 0) {
                Text(recording.title).font(.lpHeadline).foregroundStyle(Color.ink).lineLimit(1)
                Text(details(recording)).font(.lpCaption).foregroundStyle(Color.inkMuted)
            }
            Spacer(minLength: Spacing.s)
            if recording.isOnPhone {
                ShareLink(item: recording.local) {
                    Image(systemName: "square.and.arrow.up").font(.system(size: 17, weight: .semibold))
                }
                .foregroundStyle(Color.ink)
                .accessibilityLabel("Share or save")
            }
            Menu {
                Button("Play on Dotty", systemImage: "hifispeaker") { Task { await model.playOnDotty(recording) } }
                if !recording.isOnPhone {
                    Button("Save or share…", systemImage: "square.and.arrow.down") {
                        Task { await model.fetch([recording]) }
                    }
                }
                Button("Rename", systemImage: "pencil") { newTitle = recording.title; renaming = recording }
                Button("Delete", systemImage: "trash", role: .destructive) { deleting = recording }
            } label: {
                Image(systemName: "ellipsis.circle").font(.system(size: 20, weight: .semibold))
                    .frame(width: 36, height: 36)
                    .contentShape(Rectangle())
            }
            .foregroundStyle(Color.inkMuted)
        }
        .padding(.horizontal, Spacing.m)
        .padding(.vertical, Spacing.s)
    }

    private func details(_ recording: TapeModel.Recording) -> String {
        var parts = [Self.time(recording.duration), ByteCountFormatter.string(fromByteCount: Int64(recording.size), countStyle: .file)]
        if recording.isOnPhone { parts.append("on this iPhone") }
        return parts.joined(separator: " · ")
    }

    static func time(_ seconds: Double) -> String {
        let s = Int(seconds.rounded())
        return String(format: "%d:%02d", s / 60, s % 60)
    }
}
