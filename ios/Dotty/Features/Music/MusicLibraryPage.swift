import SwiftUI
import UniformTypeIdentifiers

/// Songs on Dotty: search, play, pick several to add to a playlist or delete, and send new
/// ones from Files (the outbox).
struct MusicLibraryPage: View {
    let model: MusicModel
    /// Picked songs while selecting; nil otherwise. The action bar is `librarySelectionBar`.
    @Binding var selection: Set<String>?

    @State private var search = ""
    @AppStorage("music.librarySort") private var sort = LibrarySort.name
    @State private var picking = false
    @State private var deleting: MusicModel.Song?

    private var visibleSongs: [MusicModel.Song] {
        let query = search.trimmingCharacters(in: .whitespaces)
        let found = query.isEmpty ? model.songs : model.songs.filter { $0.title.localizedCaseInsensitiveContains(query) }
        return sort.sorted(found)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Spacing.xl) {
            SendSongsCard(outbox: model.outbox, picking: $picking)
            songsCard
        }
        .fileImporter(isPresented: $picking, allowedContentTypes: [.mp3], allowsMultipleSelection: true) { result in
            if case .success(let urls) = result { model.outbox.add(urls) }
        }
        .confirmationDialog("Delete this song from Dotty?", isPresented: .init(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
                            titleVisibility: .visible, presenting: deleting) { song in
            Button("Delete \(song.title)", role: .destructive) { Task { await model.deleteSongs([song.name]) } }
        } message: { _ in
            Text("It's removed from the SD card and from every playlist.")
        }
    }

    private var songsCard: some View {
        GlassCard {
            HStack(spacing: Spacing.m) {
                Text(model.songs.count == 1 ? "1 song" : "\(model.songs.count) songs")
                    .font(.lpTitle).foregroundStyle(Color.ink)
                Spacer()
                if !model.songs.isEmpty {
                    Button(selection == nil ? "Select" : "Done") {
                        withAnimation(.settle) { selection = selection == nil ? [] : nil }
                    }
                    .buttonStyle(.quiet())
                }
            }
            .padding(.horizontal, Spacing.m)
            .padding(.top, Spacing.s)

            if model.songs.count > 1 {
                // Shows the current order; a tap switches to the other one.
                MorphButton(faces: LibrarySort.allCases.map(\.face),
                            initial: LibrarySort.allCases.firstIndex(of: sort) ?? 0) { index in
                    withAnimation(.settle) { sort = LibrarySort.allCases[index] }
                }
                .padding(.horizontal, Spacing.s)
                .padding(.top, Spacing.s)
            }

            if model.songs.count > 6 {
                HStack(spacing: Spacing.s) {
                    Image(systemName: "magnifyingglass").foregroundStyle(Color.inkMuted)
                    TextField("Search", text: $search)
                        .font(.lpBody)
                        .foregroundStyle(Color.ink)
                        .autocorrectionDisabled()
                    if !search.isEmpty {
                        Button { search = "" } label: { Image(systemName: "xmark.circle.fill") }
                            .buttonStyle(.plain)
                            .foregroundStyle(Color.inkMuted)
                    }
                }
                .padding(Spacing.m)
                .glassSurface(cornerRadius: Radius.soft)
                .padding(Spacing.s)
            }

            if model.songs.isEmpty {
                LightRow(title: "No songs yet", subtitle: "Choose some above and sync", systemImage: "music.note")
            } else if visibleSongs.isEmpty {
                LightRow(title: "No matches", systemImage: "magnifyingglass")
            }
            ForEach(visibleSongs) { song in
                if let picked = selection {
                    LightRow(title: song.title, subtitle: size(song),
                             systemImage: picked.contains(song.name) ? "checkmark.circle.fill" : "circle",
                             action: { toggle(song.name) })
                } else {
                    LightRow(title: song.title, subtitle: size(song),
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
                            Button("Select", systemImage: "checkmark.circle") { selection = [song.name] }
                            Button("Delete from Dotty", systemImage: "trash", role: .destructive) { deleting = song }
                        }
                }
            }
        }
        .sensoryFeedback(Haptic.select, trigger: selection?.count)
    }

    private func size(_ song: MusicModel.Song) -> String {
        let size = ByteCountFormatter.string(fromByteCount: Int64(song.size), countStyle: .file)
        guard sort == .added, let added = song.added else { return size }
        return "\(size) · added \(added.formatted(.relative(presentation: .named)))"
    }

    private func toggle(_ name: String) {
        guard var picked = selection else { return }
        if picked.contains(name) { picked.remove(name) } else { picked.insert(name) }
        selection = picked
    }
}

/// How the library is ordered (remembered between launches).
enum LibrarySort: String, CaseIterable {
    case name, added

    /// The sort button's face for this order (MorphButton cycles through them).
    var face: MorphFace {
        switch self {
        case .name: MorphFace(light: DottyLight.lagoon.color, title: "Sort by name", systemImage: "textformat")
        case .added: MorphFace(light: DottyLight.amber.color, title: "Sort by date", systemImage: "calendar")
        }
    }

    func sorted(_ songs: [MusicModel.Song]) -> [MusicModel.Song] {
        switch self {
        case .name:
            return songs.sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
        case .added:  // newest first; undated songs last, by name
            return songs.sorted { a, b in
                switch (a.added, b.added) {
                case let (x?, y?) where x != y: return x > y
                case (.some, nil): return true
                case (nil, .some): return false
                default: return a.title.localizedStandardCompare(b.title) == .orderedAscending
                }
            }
        }
    }
}

// MARK: - Selection actions

extension View {
    /// While library songs are being selected: a bar at the bottom to add them to a playlist
    /// (or a new one), delete them, or select all.
    func librarySelectionBar(model: MusicModel, selection: Binding<Set<String>?>) -> some View {
        modifier(LibrarySelectionBar(model: model, selection: selection))
    }
}

private struct LibrarySelectionBar: ViewModifier {
    let model: MusicModel
    @Binding var selection: Set<String>?
    @State private var naming = false
    @State private var newPlaylist = ""
    @State private var confirmDelete = false

    private var picked: [String] {
        // In library order, so playlists get them in the order shown.
        model.songs.map(\.name).filter { selection?.contains($0) == true }
    }

    func body(content: Content) -> some View {
        content
            .safeAreaInset(edge: .bottom) {
                if let selected = selection {
                    bar(count: selected.count)
                        .padding(.horizontal, Spacing.l)
                        .padding(.bottom, Spacing.s)
                        .transition(.move(edge: .bottom).combined(with: .opacity))
                }
            }
            .alert("New playlist", isPresented: $naming) {
                TextField("Name", text: $newPlaylist)
                Button("Create") {
                    let name = newPlaylist.trimmingCharacters(in: .whitespaces)
                    let songs = picked
                    newPlaylist = ""
                    guard !name.isEmpty else { return }
                    selection = nil
                    Task { await model.createPlaylist(name, with: songs) }
                }
                Button("Cancel", role: .cancel) { newPlaylist = "" }
            } message: {
                Text(picked.count == 1 ? "With the selected song." : "With the \(picked.count) selected songs.")
            }
            .confirmationDialog(picked.count == 1 ? "Delete 1 song from Dotty?" : "Delete \(picked.count) songs from Dotty?",
                                isPresented: $confirmDelete, titleVisibility: .visible) {
                Button("Delete", role: .destructive) {
                    let songs = picked
                    selection = nil
                    Task { await model.deleteSongs(songs) }
                }
            } message: {
                Text("They're removed from the SD card and from every playlist.")
            }
    }

    private func bar(count: Int) -> some View {
        let allPicked = count == model.songs.count
        return HStack(spacing: Spacing.m) {
            Button(allPicked ? "None" : "All") {
                selection = allPicked ? [] : Set(model.songs.map(\.name))
            }
            .buttonStyle(.quiet())
            Text(count == 1 ? "1 selected" : "\(count) selected")
                .font(.lpCallout.monospacedDigit())
                .foregroundStyle(Color.inkMuted)
                .lineLimit(1)
                .fixedSize()
            Spacer(minLength: 0)
            Menu {
                ForEach(model.playlists) { playlist in
                    Button(playlist.name) {
                        let songs = picked
                        selection = nil
                        Task { await model.add(songs, to: playlist.name) }
                    }
                }
                Divider()
                Button("New playlist…", systemImage: "plus") { naming = true }
            } label: {
                Label("Add to", systemImage: "text.badge.plus")
                    .font(.lpCallout.weight(.semibold))
                    .foregroundStyle(Color.onLight)
                    .padding(.horizontal, Spacing.l)
                    .frame(minHeight: 44)
                    .background(Capsule().fill(DottyLight.firefly.color))
            }
            .disabled(count == 0)
            Button { confirmDelete = true } label: { Image(systemName: "trash") }
                .buttonStyle(.quiet(DottyLight.ember.color))
                .disabled(count == 0)
                .accessibilityLabel("Delete selected songs")
        }
        .padding(Spacing.s)
        .glassSurface(cornerRadius: 32, frosted: true)
    }
}

// MARK: - Sending songs

/// The queue of songs to send to Dotty over Wi-Fi, with Sync, progress and the hotspot hint.
private struct SendSongsCard: View {
    let outbox: SongOutbox
    @Binding var picking: Bool

    var body: some View {
        GlassCard(title: "Send songs") {
            if let notice = outbox.notice {
                NoticeCard(kind: .success, text: notice).padding(.horizontal, Spacing.s).padding(.bottom, Spacing.s)
            }
            if let error = outbox.error {
                NoticeCard(kind: .error, text: error).padding(.horizontal, Spacing.s).padding(.bottom, Spacing.s)
            }
            if let weak = outbox.weakSignal {
                weakSignalHint(ssid: weak.ssid, rssi: weak.rssi)
            }
            if outbox.uploads.isEmpty {
                LightRow(title: "Choose songs", subtitle: "MP3 files from the Files app", systemImage: "plus",
                         action: { picking = true })
            } else {
                ForEach(outbox.uploads) { upload in
                    LightRow(title: upload.name, subtitle: subtitle(upload), systemImage: symbol(upload)) {
                        if !outbox.syncing && upload.state != .sending {
                            Button { outbox.remove(upload) } label: { Image(systemName: "xmark") }
                                .buttonStyle(.quiet(DottyLight.ember.color))
                        }
                    }
                }
                if outbox.syncing {
                    VStack(alignment: .leading, spacing: Spacing.s) {
                        LightProgress(value: outbox.progress)
                        Text(outbox.syncStage ?? "").font(.lpCaption).foregroundStyle(Color.inkMuted)
                        if let detail = outbox.detail {
                            Text(detail).font(.lpCaption.monospacedDigit()).foregroundStyle(Color.inkMuted)
                        }
                        Text("Keep this app open until it's done; your screen stays on.")
                            .font(.lpCaption).foregroundStyle(Color.inkFaint)
                    }
                    .padding(Spacing.m)
                } else {
                    HStack(spacing: Spacing.m) {
                        Button("Sync \(outbox.waitingCount) to Dotty") { Task { await outbox.sync() } }
                            .buttonStyle(.light())
                        Button("Add more") { picking = true }
                            .buttonStyle(.quiet())
                    }
                    .padding(Spacing.m)
                }
            }
        }
    }

    /// Dotty's antenna is small: far from the router uploads crawl, next to a hotspot they fly.
    private func weakSignalHint(ssid: String, rssi: Int) -> some View {
        VStack(alignment: .leading, spacing: Spacing.s) {
            NoticeCard(kind: .info, text: "Dotty's Wi-Fi is weak on \(ssid) (\(rssi) dBm), so songs go slowly. Your iPhone's Personal Hotspot next to Dotty is about 3× faster: turn it on (with Maximize Compatibility), add it under Wi-Fi, and Dotty picks it whenever it's the strongest.")
            NavigationLink(value: DashboardView.Route.wifi) {
                Label("Wi-Fi networks", systemImage: "wifi")
            }
            .buttonStyle(.quiet())
        }
        .padding(.horizontal, Spacing.s)
        .padding(.bottom, Spacing.s)
    }

    private func subtitle(_ upload: SongOutbox.Upload) -> String {
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

    private func symbol(_ upload: SongOutbox.Upload) -> String {
        switch upload.state {
        case .waiting: "music.note"
        case .sending: "arrow.up.circle"
        case .sent: "checkmark.circle"
        case .failed: "exclamationmark.triangle"
        }
    }
}
