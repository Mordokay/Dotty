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
    @State private var editMode = EditMode.inactive

    init(model: MusicModel, name: String) {
        self.model = model
        _name = State(initialValue: name)
    }

    var body: some View {
        LightField {
            List {
                header
                    .moveDisabled(true)
                    .deleteDisabled(true)
                    .plainRow()
                if loading {
                    HStack { Spacer(); FireflyLoader(size: 64, label: "Loading"); Spacer() }
                        .padding(.vertical, Spacing.l)
                        .moveDisabled(true)
                        .deleteDisabled(true)
                        .plainRow()
                } else if songs.isEmpty {
                    LightRow(title: "No songs yet", subtitle: "Add some from your library", systemImage: "music.note")
                        .glassSurface(cornerRadius: Radius.soft)
                        .moveDisabled(true)
                        .deleteDisabled(true)
                        .plainRow()
                }
                ForEach(Array(songs.enumerated()), id: \.element) { index, song in
                    LightRow(title: title(song), subtitle: "\(index + 1)",
                             systemImage: model.now.queue == name && model.now.song == song && model.now.playing
                                 ? "speaker.wave.2.fill" : "music.note",
                             action: editMode.isEditing ? nil : { Task { await model.play(playlist: name, song: song) } })
                        .glassSurface(cornerRadius: Radius.soft)
                        .plainRow()
                }
                .onMove(perform: move)
                .onDelete(perform: remove)
                footer
                    .moveDisabled(true)
                    .deleteDisabled(true)
                    .plainRow()
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .environment(\.editMode, $editMode)
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
            if songs.count > 1 || editMode.isEditing {
                HStack(spacing: Spacing.s) {
                    Spacer()
                    Menu {
                        Button("Name (A–Z)", systemImage: "textformat") { Task { await model.sort(name, byDateAdded: false) } }
                        Button("Date added (newest first)", systemImage: "calendar") { Task { await model.sort(name, byDateAdded: true) } }
                    } label: {
                        Label("Sort", systemImage: "arrow.up.arrow.down")
                            .font(.lpCallout.weight(.semibold))
                            .foregroundStyle(Color.inkMuted)
                            .frame(minHeight: 40)
                    }
                    .disabled(editMode.isEditing)
                    Button(editMode.isEditing ? "Done" : "Edit") {
                        withAnimation { editMode = editMode.isEditing ? .inactive : .active }
                    }
                    .buttonStyle(.quiet())
                }
            }
            if editMode.isEditing {
                Text("Drag ≡ to reorder, tap ⊖ to remove a song. Sort ↑↓ puts the whole list in order.")
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
    private func move(from source: IndexSet, to destination: Int) {
        guard let from = source.first else { return }
        songs.move(fromOffsets: source, toOffset: destination)
        let to = destination > from ? destination - 1 : destination
        guard to != from else { return }
        Task { await model.move(in: name, from: from, to: to) }
    }

    private func remove(at offsets: IndexSet) {
        let removed = offsets.map { songs[$0] }
        songs.remove(atOffsets: offsets)
        Task { for song in removed { await model.remove(song, from: name) } }
    }

    private func title(_ song: String) -> String {
        model.songs.first { $0.name == song }?.title ?? song
    }
}

private extension View {
    /// A list row without the list's own background, separator or insets.
    func plainRow() -> some View {
        listRowBackground(Color.clear)
            .listRowSeparator(.hidden)
            .listRowInsets(EdgeInsets(top: Spacing.xs, leading: Spacing.l, bottom: Spacing.xs, trailing: Spacing.l))
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
