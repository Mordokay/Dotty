import SwiftUI
import UniformTypeIdentifiers

/// The Music cartridge's screen: remote, songs to send over Wi-Fi, playlists and the library.
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

private struct MusicContent: View {
    @Bindable var model: MusicModel
    @Environment(DottyLink.self) private var link
    @State private var volume: Double = 80
    @State private var volumeTask: Task<Void, Never>?
    @State private var picking = false
    @State private var naming = false
    @State private var newPlaylist = ""
    @State private var deleting: MusicModel.Song?
    @State private var renaming: String?
    @State private var newName = ""

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Music", subtitle: "Songs live on Dotty's SD card. Send new ones from Files over your Wi-Fi.")
                        .padding(.top, Spacing.l)
                    if let notice = model.notice { NoticeCard(kind: .success, text: notice) }
                    if let error = model.error { NoticeCard(kind: .error, text: error) }
                    if !model.loaded && model.error == nil {
                        HStack { Spacer(); FireflyLoader(size: 96, label: "Reading Dotty's library"); Spacer() }
                            .padding(.top, Spacing.xxl)
                    } else {
                        nowPlayingCard
                        sendCard
                        playlistsCard
                        libraryCard
                    }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await model.load() }
        }
        .navigationTitle("")
        .navigationDestination(for: MusicModel.Playlist.self) { playlist in
            PlaylistView(model: model, name: playlist.name)
        }
        .task { await model.load() }
        .onChange(of: link.connection) { _, state in
            if state == .connected { Task { await model.reconnected() } }
        }
        .onChange(of: link.eventCount) { _, _ in
            if let event = link.lastEvent { model.handle(event) }
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
        .fileImporter(isPresented: $picking, allowedContentTypes: [.mp3], allowsMultipleSelection: true) { result in
            if case .success(let urls) = result { model.queue(urls) }
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
        .confirmationDialog("Delete this song from Dotty?", isPresented: .init(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
                            titleVisibility: .visible, presenting: deleting) { song in
            Button("Delete \(song.title)", role: .destructive) { Task { await model.deleteSong(song.name) } }
        } message: { _ in
            Text("It's removed from the SD card and from every playlist.")
        }
    }

    // MARK: - Now playing

    private var nowPlayingCard: some View {
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
                HStack(spacing: Spacing.xl) {
                    Button { Task { await model.setShuffle(!model.now.shuffle) } } label: {
                        Image(systemName: model.now.shuffle ? "shuffle" : "arrow.right")
                    }
                    .buttonStyle(.quiet(model.now.shuffle ? DottyLight.firefly.color : .inkMuted))
                    .accessibilityLabel(model.now.shuffle ? "Shuffle on" : "Shuffle off")
                    Spacer()
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
                    Spacer()
                    // Balances the shuffle button so the transport stays centred.
                    Image(systemName: "shuffle").hidden().padding(.horizontal, Spacing.m)
                }
                .disabled(model.songs.isEmpty)
                LightSlider(title: "Volume", value: $volume)
            }
            .padding(.horizontal, Spacing.m)
            .padding(.bottom, Spacing.m)
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

    // MARK: - Sending songs

    private var sendCard: some View {
        GlassCard(title: "Send songs") {
            if model.uploads.isEmpty {
                LightRow(title: "Choose songs", subtitle: "MP3 files from the Files app", systemImage: "plus",
                         action: { picking = true })
            } else {
                ForEach(model.uploads) { upload in
                    LightRow(title: upload.name, subtitle: subtitle(upload), systemImage: symbol(upload)) {
                        if !model.syncing && upload.state != .sending {
                            Button { model.unqueue(upload) } label: { Image(systemName: "xmark") }
                                .buttonStyle(.quiet(DottyLight.ember.color))
                        }
                    }
                }
                if model.syncing {
                    VStack(alignment: .leading, spacing: Spacing.s) {
                        LightProgress(value: model.syncProgress)
                        Text(model.syncStage ?? "").font(.lpCaption).foregroundStyle(Color.inkMuted)
                        if let detail = model.syncDetail {
                            Text(detail).font(.lpCaption.monospacedDigit()).foregroundStyle(Color.inkMuted)
                        }
                    }
                    .padding(Spacing.m)
                } else {
                    HStack(spacing: Spacing.m) {
                        Button("Sync \(model.waitingCount) to Dotty") { Task { await model.sync() } }
                            .buttonStyle(.light())
                        Button("Add more") { picking = true }
                            .buttonStyle(.quiet())
                    }
                    .padding(Spacing.m)
                }
            }
        }
    }

    private func subtitle(_ upload: MusicModel.Upload) -> String {
        let size = ByteCountFormatter.string(fromByteCount: upload.size, countStyle: .file)
        switch upload.state {
        case .waiting: return size
        case .sending:
            let percent = upload.size > 0 ? Int(Double(upload.sent) / Double(upload.size) * 100) : 0
            let sent = ByteCountFormatter.string(fromByteCount: upload.sent, countStyle: .file)
            return "\(sent) of \(size) · \(percent)%"
        case .sent: return "On Dotty"
        case .failed: return "Didn't arrive · \(size)"
        }
    }

    private func symbol(_ upload: MusicModel.Upload) -> String {
        switch upload.state {
        case .waiting: "music.note"
        case .sending: "arrow.up.circle"
        case .sent: "checkmark.circle"
        case .failed: "exclamationmark.triangle"
        }
    }

    // MARK: - Playlists and library

    private var playlistsCard: some View {
        GlassCard(title: "Playlists") {
            ForEach(model.playlists) { playlist in
                NavigationLink(value: playlist) {
                    LightRow(title: playlist.name, subtitle: playlist.count == 1 ? "1 song" : "\(playlist.count) songs",
                             systemImage: "music.note.list") {
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
    }

    private var libraryCard: some View {
        GlassCard(title: "Library") {
            if model.songs.isEmpty {
                LightRow(title: "No songs yet", subtitle: "Choose some above and sync", systemImage: "music.note")
            }
            ForEach(model.songs) { song in
                LightRow(title: song.title,
                         subtitle: ByteCountFormatter.string(fromByteCount: Int64(song.size), countStyle: .file),
                         systemImage: model.now.song == song.name && model.now.playing ? "speaker.wave.2.fill" : "music.note",
                         action: { Task { await model.play(song: song.name) } })
                    .contextMenu {
                        Button("Play", systemImage: "play") { Task { await model.play(song: song.name) } }
                        if !model.playlists.isEmpty {
                            Menu("Add to playlist", systemImage: "text.badge.plus") {
                                ForEach(model.playlists) { playlist in
                                    Button(playlist.name) { Task { await model.add([song.name], to: playlist.name) } }
                                }
                            }
                        }
                        Button("Delete from Dotty", systemImage: "trash", role: .destructive) { deleting = song }
                    }
            }
        }
    }
}
