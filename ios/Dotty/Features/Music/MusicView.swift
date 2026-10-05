import SwiftUI

/// The Music cartridge's screen, in three pages: the player (remote), the library (songs on
/// Dotty, sending new ones, multi-select) and playlists.
struct MusicView: View {
    @Environment(DottyLink.self) private var link
    @State private var model: MusicModel?

    var body: some View {
        Group {
            if let model {
                MusicContent(model: model)
            } else {
                LightField { Color.clear }
            }
        }
        .onAppear { if model == nil { model = MusicModel(link: link) } }
    }
}

enum MusicPage: Hashable, CaseIterable {
    case player, library, playlists

    var title: String {
        switch self {
        case .player: "Player"
        case .library: "Library"
        case .playlists: "Playlists"
        }
    }
}

private struct MusicContent: View {
    @Bindable var model: MusicModel
    @Environment(DottyLink.self) private var link
    @State private var page = MusicPage.player
    /// Library songs picked for a bulk action; nil when not selecting.
    @State private var selection: Set<String>?

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Music")
                        .padding(.top, Spacing.l)
                    LightTabs(tabs: MusicPage.allCases, selection: $page) { $0.title }
                    // Bluetooth is paused on purpose while songs go over Wi-Fi.
                    if !model.outbox.syncing { NotConnectedNotice() }
                    if let notice = model.notice { NoticeCard(kind: .success, text: notice) }
                    if let error = model.error { NoticeCard(kind: .error, text: error) }
                    if !model.loaded && model.error == nil {
                        // Nothing to show until the library has loaded once (the notice above
                        // explains a missing connection).
                        if link.connection == .connected {
                            HStack { Spacer(); FireflyLoader(size: 96, label: "Reading Dotty's library"); Spacer() }
                                .padding(.top, Spacing.xxl)
                        }
                    } else {
                        switch page {
                        case .player: MusicPlayerCard(model: model)
                        case .library: MusicLibraryPage(model: model, selection: $selection)
                        case .playlists: MusicPlaylistsPage(model: model)
                        }
                    }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await model.load() }
            .librarySelectionBar(model: model, selection: $selection)
        }
        .navigationTitle("")
        .navigationDestination(for: MusicModel.Playlist.self) { playlist in
            PlaylistView(model: model, name: playlist.name)
        }
        .task { await model.load() }
        .onChange(of: page) { _, _ in selection = nil }
        .onChange(of: link.connection) { _, state in
            if state == .connected { Task { await model.reconnected() } }
        }
        .onChange(of: link.eventCount) { _, _ in
            if let event = link.lastEvent { model.handle(event) }
        }
    }
}

// MARK: - Player

/// What's playing, transport, shuffle and volume.
private struct MusicPlayerCard: View {
    let model: MusicModel
    @State private var volume: Double = 80
    @State private var volumeTask: Task<Void, Never>?

    var body: some View {
        GlassCard(title: "Now playing", light: model.now.playing ? DottyLight.firefly.color : nil) {
            VStack(alignment: .leading, spacing: Spacing.m) {
                VStack(alignment: .leading, spacing: Spacing.xs) {
                    Text(model.now.title.isEmpty ? "Nothing yet" : model.now.title)
                        .font(.lpHeadline)
                        .foregroundStyle(Color.ink)
                        .lineLimit(2)
                    Text(queueLine).font(.lpCaption).foregroundStyle(Color.inkMuted)
                }
                TimelineView(.periodic(from: .now, by: 1)) { context in
                    let position = model.now.position(at: context.date)
                    VStack(spacing: Spacing.xs) {
                        LightProgress(value: model.now.duration > 0 ? position / Double(model.now.duration) : 0)
                        HStack {
                            Text(Self.time(position))
                            Spacer()
                            Text(Self.time(Double(model.now.duration)))
                        }
                        .font(.lpCaption.monospacedDigit())
                        .foregroundStyle(Color.inkMuted)
                    }
                }
                // Transport centred; shuffle sits on the leading edge without widening the row.
                HStack(spacing: Spacing.l) {
                    Button { Task { await model.previous() } } label: { Image(systemName: "backward.fill") }
                        .buttonStyle(.frostedCircle)
                    Button { Task { await model.toggle() } } label: {
                        Image(systemName: model.now.playing ? "pause.fill" : "play.fill")
                            .font(.system(size: 26, weight: .bold))
                            .frame(width: 44, height: 44)
                    }
                    .buttonStyle(.light(circle: true))
                    Button { Task { await model.next() } } label: { Image(systemName: "forward.fill") }
                        .buttonStyle(.frostedCircle)
                }
                .frame(maxWidth: .infinity)
                .overlay(alignment: .leading) {
                    Button { Task { await model.setShuffle(!model.now.shuffle) } } label: {
                        Image(systemName: model.now.shuffle ? "shuffle" : "arrow.right")
                            .font(.system(size: 17, weight: .semibold))
                            .frame(width: 44, height: 44)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(model.now.shuffle ? DottyLight.firefly.color : Color.inkMuted)
                    .accessibilityLabel(model.now.shuffle ? "Shuffle on" : "Shuffle off")
                }
                .disabled(model.songs.isEmpty)
                LightSlider(title: "Volume", value: $volume)
            }
            .padding(.horizontal, Spacing.m)
            .padding(.bottom, Spacing.m)
        }
        .onChange(of: model.now.volume, initial: true) { _, value in
            if volumeTask == nil { volume = Double(value) }
        }
        .onChange(of: volume) { _, value in
            guard Int(value) != model.now.volume else { return }
            volumeTask?.cancel()
            volumeTask = Task {
                try? await Task.sleep(for: .milliseconds(250))
                guard !Task.isCancelled else { return }
                await model.setVolume(Int(value))
                volumeTask = nil
            }
        }
    }

    private var queueLine: String {
        guard model.now.count > 0 else { return model.songs.isEmpty ? "Send some songs to Dotty first" : "Tap play to start" }
        let list = model.now.queue.isEmpty ? "Library" : model.now.queue
        return "\(list) · \(model.now.index + 1) of \(model.now.count)"
    }

    private static func time(_ seconds: Double) -> String {
        let s = Int(seconds)
        return String(format: "%d:%02d", s / 60, s % 60)
    }
}

// MARK: - Playlists

private struct MusicPlaylistsPage: View {
    let model: MusicModel
    @State private var naming = false
    @State private var newPlaylist = ""
    @State private var renaming: String?
    @State private var newName = ""

    var body: some View {
        GlassCard {
            if model.playlists.isEmpty {
                LightRow(title: "No playlists yet", subtitle: "Make one, then add songs from the Library",
                         systemImage: "music.note.list")
            }
            ForEach(model.playlists) { playlist in
                NavigationLink(value: playlist) {
                    LightRow(title: playlist.name, subtitle: playlist.count == 1 ? "1 song" : "\(playlist.count) songs",
                             systemImage: model.now.queue == playlist.name && model.now.playing ? "speaker.wave.2.fill" : "music.note.list") {
                        Image(systemName: "chevron.right").font(.system(size: 14, weight: .semibold, design: .rounded))
                    }
                }
                .buttonStyle(.plain)
                .contextMenu {
                    Button("Play", systemImage: "play") { Task { await model.play(playlist: playlist.name) } }
                    Button("Rename", systemImage: "pencil") { renaming = playlist.name; newName = playlist.name }
                    Button("Delete playlist", systemImage: "trash", role: .destructive) {
                        Task { await model.deletePlaylist(playlist.name) }
                    }
                }
            }
            LightRow(title: "New playlist", systemImage: "plus", action: { naming = true })
        }
        .alert("New playlist", isPresented: $naming) {
            TextField("Name", text: $newPlaylist)
            Button("Create") {
                let name = newPlaylist.trimmingCharacters(in: .whitespaces)
                newPlaylist = ""
                if !name.isEmpty { Task { await model.createPlaylist(name) } }
            }
            Button("Cancel", role: .cancel) { newPlaylist = "" }
        }
        .alert("Rename playlist", isPresented: .init(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            TextField("Name", text: $newName)
            Button("Rename") {
                let name = newName.trimmingCharacters(in: .whitespaces)
                if let old = renaming, !name.isEmpty, name != old { Task { _ = await model.renamePlaylist(old, to: name) } }
                renaming = nil
            }
            Button("Cancel", role: .cancel) { renaming = nil }
        }
    }
}
