import SwiftUI

/// One playlist: play it, add songs from the library, drag to reorder, swipe (or Edit) to
/// remove songs.
struct PlaylistView: View {
    let model: MusicModel
    @State private var name: String

    @Environment(\.dismiss) private var dismiss
    @State private var songs: [String] = []
    @State private var loading = true
    @State private var adding = false
    @State private var confirmDelete = false
    @State private var renaming = false
    @State private var newName = ""

    init(model: MusicModel, name: String) {
        self.model = model
        _name = State(initialValue: name)
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.l) {
                    header
                    if loading {
                        HStack { Spacer(); FireflyLoader(size: 64, label: "Loading"); Spacer() }
                            .padding(.vertical, Spacing.l)
                    } else if songs.isEmpty {
                        LightRow(title: "No songs yet", subtitle: "Add some from your library", systemImage: "music.note")
                            .glassSurface(cornerRadius: Radius.soft)
                    } else {
                        ReorderableSongList(
                            songs: songs,
                            title: title,
                            isPlaying: { model.now.queue == name && model.now.song == $0 && model.now.playing },
                            onPlay: { song in Task { await model.play(playlist: name, song: song) } },
                            onMove: move,
                            onRemove: remove)
                    }
                    footer
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
        .alert("Rename playlist", isPresented: $renaming) {
            TextField("Name", text: $newName)
            Button("Rename") {
                let new = newName.trimmingCharacters(in: .whitespaces)
                guard !new.isEmpty, new != name else { return }
                Task { if await model.renamePlaylist(name, to: new) { name = new } }
            }
            Button("Cancel", role: .cancel) {}
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

    private var header: some View {
        VStack(alignment: .leading, spacing: Spacing.l) {
            PageHeader(title: name, subtitle: songs.count == 1 ? "1 song" : "\(songs.count) songs")
                .padding(.top, Spacing.l)
            HStack(spacing: Spacing.m) {
                Button("Play", systemImage: "play.fill") { Task { await model.play(playlist: name) } }
                    .buttonStyle(.light())
                    .disabled(songs.isEmpty)
                Button("Add songs", systemImage: "plus") { adding = true }
                    .buttonStyle(.quiet())
                    .disabled(model.songs.isEmpty)
            }
            if songs.count > 1 {
                // Shows how the playlist was last sorted; a tap re-sorts it the other way.
                MorphButton(faces: LibrarySort.allCases.map(\.face),
                            initial: LibrarySort.allCases.firstIndex(of: lastSort) ?? 0) { index in
                    let order = LibrarySort.allCases[index]
                    UserDefaults.standard.set(order.rawValue, forKey: sortKey)
                    Task { await model.sort(name, byDateAdded: order == .added) }
                }
                Text("Drag ≡ to reorder · swipe left to remove")
                    .font(.lpCaption).foregroundStyle(Color.inkMuted)
            }
        }
    }

    private var footer: some View {
        HStack(spacing: Spacing.m) {
            Button("Rename", systemImage: "pencil") { newName = name; renaming = true }
                .buttonStyle(.quiet())
            Button("Delete playlist", systemImage: "trash") { confirmDelete = true }
                .buttonStyle(.quiet(DottyLight.ember.color))
        }
        .padding(.top, Spacing.l)
        .padding(.bottom, Spacing.xxl)
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
