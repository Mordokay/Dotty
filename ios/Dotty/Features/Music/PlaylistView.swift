import SwiftUI

/// One playlist: a panel with its name (edited in place), song count and sort; a lit
/// "Add songs" first row; songs with play/pause on the left and a ≡ handle on the right,
/// swipe left to remove; a jelly Delete button at the bottom.
struct PlaylistView: View {
    let model: MusicModel
    @State private var name: String

    @Environment(\.dismiss) private var dismiss
    @State private var songs: [String] = []
    @State private var loading = true
    @State private var adding = false
    @State private var confirmDelete = false
    /// The name field: locked until the name or the pencil is tapped.
    @State private var editingName = false
    @State private var draftName: String
    @FocusState private var nameFocused: Bool

    init(model: MusicModel, name: String) {
        self.model = model
        _name = State(initialValue: name)
        _draftName = State(initialValue: name)
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.l) {
                    titlePanel
                        .padding(.top, Spacing.l)
                    if loading {
                        HStack { Spacer(); FireflyLoader(size: 64, label: "Loading"); Spacer() }
                            .padding(.vertical, Spacing.l)
                    } else {
                        VStack(spacing: ReorderableSongList.spacing) {
                            addSongsRow
                            ReorderableSongList(
                                songs: songs,
                                title: title,
                                playState: playState,
                                onPlay: playOrPause,
                                onMove: move,
                                onRemove: remove)
                        }
                    }
                    MorphButton(faces: [MorphFace(light: DottyLight.ember.color, title: "Delete playlist", systemImage: "trash")]) { _ in
                        confirmDelete = true
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.top, Spacing.l)
                    .padding(.bottom, Spacing.xxl)
                }
                .padding(.horizontal, Spacing.l)
            }
        }
        .navigationTitle("")
        .task(id: model.libraryVersion) {
            songs = await model.playlistSongs(name)
            loading = false
        }
        .sheet(isPresented: $adding) {
            AddSongsSheet(songs: model.songs.filter { !songs.contains($0.name) }) { picked in
                Task { await model.add(picked, to: name) }
            }
            .presentationDetents([.large])
            .presentationBackground(.clear)
        }
        .onChange(of: nameFocused) { _, focused in
            if !focused && editingName { commitName() }
        }
        .confirmationDialog("Delete \(name)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete playlist", role: .destructive) {
                Task {
                    await model.deletePlaylist(name)
                    dismiss()
                }
            }
        } message: {
            Text("The songs stay in your library.")
        }
    }

    // MARK: - Title panel

    /// Name (editable in place) with its pencil, the song count, and the sort button.
    private var titlePanel: some View {
        HStack(alignment: .center, spacing: Spacing.m) {
            VStack(alignment: .leading, spacing: Spacing.xs) {
                nameRow
                Text(songs.count == 1 ? "1 song" : "\(songs.count) songs")
                    .font(.lpCallout)
                    .foregroundStyle(Color.inkMuted)
            }
            if songs.count > 1 { sortButton }
        }
        .padding(Spacing.l)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassSurface()
    }

    /// The name is a text field, locked until the name or the pencil is tapped.
    private var nameRow: some View {
        HStack(spacing: Spacing.s) {
            TextField("Playlist name", text: $draftName)
                .font(.lpTitle)
                .foregroundStyle(Color.ink)
                .focused($nameFocused)
                .submitLabel(.done)
                .onSubmit { commitName() }
                .allowsHitTesting(editingName)
            pencilButton
        }
        .contentShape(Rectangle())
        .onTapGesture {
            if !editingName { startEditing() }
        }
    }

    private var pencilButton: some View {
        Button {
            if editingName { commitName() } else { startEditing() }
        } label: {
            Image(systemName: editingName ? "checkmark.circle.fill" : "pencil")
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(editingName ? DottyLight.firefly.color : Color.inkMuted)
                .frame(width: 36, height: 36)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(editingName ? "Save name" : "Rename playlist")
    }

    /// Shows how the playlist was last sorted; a tap re-sorts it the other way.
    private var sortButton: some View {
        let faces: [MorphFace] = LibrarySort.allCases.map(\.shortFace)
        let initial = LibrarySort.allCases.firstIndex(of: lastSort) ?? 0
        return MorphButton(faces: faces, initial: initial) { index in
            let order = LibrarySort.allCases[index]
            UserDefaults.standard.set(order.rawValue, forKey: sortKey)
            Task { await model.sort(name, byDateAdded: order == .added) }
        }
        .fixedSize()
    }

    private func startEditing() {
        draftName = name
        editingName = true
        nameFocused = true
    }

    private func commitName() {
        editingName = false
        nameFocused = false
        let new = draftName.trimmingCharacters(in: .whitespaces)
        guard !new.isEmpty, new != name else {
            draftName = name
            return
        }
        Task {
            if await model.renamePlaylist(name, to: new) {
                UserDefaults.standard.set(UserDefaults.standard.string(forKey: sortKey), forKey: "music.playlistSort.\(new)")
                name = new
            } else {
                draftName = name
            }
        }
    }

    // MARK: - Rows

    /// The list's first row: lit, and never moved or removed.
    private var addSongsRow: some View {
        Button { adding = true } label: {
            HStack(spacing: Spacing.m) {
                Image(systemName: "plus.circle.fill")
                    .font(.system(size: 20, weight: .semibold))
                    .foregroundStyle(DottyLight.firefly.color)
                Text(songs.isEmpty ? "Add songs from your library" : "Add songs")
                    .font(.lpHeadline)
                    .foregroundStyle(Color.ink)
                Spacer()
            }
            .padding(.horizontal, Spacing.l)
            .frame(height: ReorderableSongList.rowHeight)
            .glassSurface(light: DottyLight.firefly.color, cornerRadius: Radius.soft)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(model.songs.isEmpty)
        .opacity(model.songs.isEmpty ? 0.5 : 1)
    }

    private func playState(_ song: String) -> ReorderableSongList.PlayState {
        guard model.now.queue == name, model.now.song == song, model.now.count > 0 else { return .idle }
        if model.now.playing { return .playing }
        return model.now.paused ? .paused : .idle
    }

    /// Pauses or resumes the song playing here; any other song starts this playlist from it.
    private func playOrPause(_ song: String) {
        Task {
            switch playState(song) {
            case .playing, .paused: await model.toggle()
            case .idle: await model.play(playlist: name, song: song)
            }
        }
    }

    /// Reorders here at once, then on Dotty (which reloads the list when done).
    private func move(from: Int, to: Int) {
        withAnimation(.settle) { songs.insert(songs.remove(at: from), at: to) }
        Task { await model.move(in: name, from: from, to: to) }
    }

    private func remove(_ song: String) {
        withAnimation(.settle) { songs.removeAll { $0 == song } }
        Task { await model.remove(song, from: name) }
    }

    private var sortKey: String { "music.playlistSort.\(name)" }

    /// The order last applied to this playlist from this phone (name by default).
    private var lastSort: LibrarySort {
        UserDefaults.standard.string(forKey: sortKey).flatMap(LibrarySort.init(rawValue:)) ?? .name
    }

    private func title(_ song: String) -> String {
        model.songs.first { $0.name == song }?.title ?? song
    }
}

/// Pick library songs to add to a playlist.
private struct AddSongsSheet: View {
    let songs: [MusicModel.Song]
    var onAdd: ([String]) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var picked: Set<String> = []

    var body: some View {
        VStack(alignment: .leading, spacing: Spacing.l) {
            Text("Add songs").font(.lpTitle).foregroundStyle(Color.ink)
            ScrollView {
                VStack(spacing: 0) {
                    if songs.isEmpty {
                        LightRow(title: "Every song is already here", systemImage: "checkmark.circle")
                    }
                    ForEach(songs) { song in
                        LightRow(title: song.title, systemImage: picked.contains(song.name) ? "checkmark.circle.fill" : "circle",
                                 action: { toggle(song.name) })
                    }
                }
            }
            HStack(spacing: Spacing.m) {
                Button(picked.isEmpty ? "Add" : "Add \(picked.count)") {
                    onAdd(songs.map(\.name).filter(picked.contains))
                    dismiss()
                }
                .buttonStyle(.light())
                .disabled(picked.isEmpty)
                Button("Cancel") { dismiss() }.buttonStyle(.quiet())
            }
        }
        .padding(Spacing.xl)
        .glassSurface(cornerRadius: Radius.sheet, frosted: true)
        .padding(Spacing.s)
    }

    private func toggle(_ name: String) {
        if picked.contains(name) { picked.remove(name) } else { picked.insert(name) }
    }
}
