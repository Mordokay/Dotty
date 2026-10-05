import PhotosUI
import SwiftUI

/// One album ("" = all photos): show it on Dotty or its lock screen, add photos (new ones
/// from the iPhone, or ones already on Dotty), drag to reorder, remove, rename, delete.
struct AlbumDetailView: View {
    let model: AlbumModel
    let name: String
    @Environment(DottyLink.self) private var link
    @Environment(\.dismiss) private var dismiss
    @State private var photos: [String] = []
    @State private var title: String
    @State private var picking = false
    @State private var picked: [PhotosPickerItem] = []
    @State private var editor: EditorRequest?
    @State private var choosingFromDotty = false
    @State private var detail: AlbumModel.Photo?
    @State private var renaming = false
    @State private var newName = ""
    @State private var deleting: String?
    @State private var selection: Set<String>?

    init(model: AlbumModel, name: String) {
        self.model = model
        self.name = name
        _title = State(initialValue: name)
    }

    private var isAll: Bool { name.isEmpty }
    private var isActive: Bool { model.onDotty.album == title }
    private var onLockScreen: Bool { isActive && model.onDotty.lockPhoto == nil }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: isAll ? "All photos" : title,
                               subtitle: photos.count == 1 ? "1 photo" : "\(photos.count) photos")
                        .padding(.top, Spacing.l)
                    if !model.outbox.syncing { NotConnectedNotice() }
                    GlassCard {
                        LightRow(title: "Show on Dotty", subtitle: isActive ? "Dotty is showing this album" : nil,
                                 systemImage: "photo") {
                            Button("Show") { Task { await model.show(album: title) } }.buttonStyle(.quiet())
                        }
                        ChoiceRow(title: "Lock screen slideshow", subtitle: "A new photo every minute, looping",
                                  systemImage: "lock", chosen: onLockScreen) {
                            Task { await model.lockScreen(album: title) }
                        }
                    }
                    .needsDotty(link)
                    GlassCard {
                        if !isAll {
                            LightRow(title: "Add new photos", subtitle: "From your iPhone; you frame each one",
                                     systemImage: "plus", action: { picking = true })
                            LightRow(title: "Add photos from Dotty", subtitle: "Ones already on Dotty",
                                     systemImage: "rectangle.stack.badge.plus", action: { choosingFromDotty = true })
                        }
                        if photos.isEmpty {
                            Text(isAll ? "No photos on Dotty yet." : "No photos in this album yet.")
                                .font(.lpCallout).foregroundStyle(Color.inkMuted)
                                .padding(Spacing.m)
                        } else {
                            if !isAll {
                                Text("Drag a photo onto another to move it there. Touch and hold for more.")
                                    .font(.lpCaption).foregroundStyle(Color.inkFaint)
                                    .padding(.horizontal, Spacing.m).padding(.top, Spacing.s)
                            }
                            PhotoGrid(model: model, names: photos, selection: $selection, onOpen: { photo in
                                detail = model.photos.first { $0.name == photo }
                            }, onMove: isAll ? nil : { from, to in move(from, to) })
                            .padding(Spacing.s)
                        }
                    }
                    .needsDotty(link)
                    if !isAll {
                        GlassCard {
                            LightRow(title: "Rename album", systemImage: "pencil", action: { newName = title; renaming = true })
                            LightRow(title: "Delete album", subtitle: "Its photos stay on Dotty", systemImage: "trash",
                                     action: { deleting = title })
                        }
                        .needsDotty(link)
                    }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await reload() }
        }
        .navigationTitle("")
        .task(id: model.libraryVersion) { await reload() }
        .photosPicker(isPresented: $picking, selection: $picked, maxSelectionCount: 30, matching: .images)
        .onChange(of: picked) { _, items in
            guard !items.isEmpty else { return }
            editor = EditorRequest(items: items, album: title)
            picked = []
        }
        .photoEditorSheet(model: model, request: $editor)
        .sheet(item: $detail) { photo in
            AlbumPhotoSheet(model: model, photo: photo, album: isAll ? nil : title)
                .presentationDetents([.large])
                .presentationBackground(.clear)
        }
        .sheet(isPresented: $choosingFromDotty) {
            ChooseFromDotty(model: model, excluded: Set(photos)) { names in
                Task { await model.add(names, to: title) }
            }
            .presentationDetents([.large])
            .presentationBackground(.clear)
        }
        .alert("Rename album", isPresented: $renaming) {
            TextField("Name", text: $newName)
            Button("Rename") {
                let new = newName.trimmingCharacters(in: .whitespaces)
                guard !new.isEmpty, new != title else { return }
                Task { if await model.renameAlbum(title, to: new) { title = new } }
            }
            Button("Cancel", role: .cancel) {}
        }
        .deleteAlbumDialog(model: model, name: $deleting) { dismiss() }
    }

    private func reload() async {
        photos = await model.albumPhotos(title)
    }

    private func move(_ from: Int, _ to: Int) {
        withAnimation(.settle) {
            let moved = photos.remove(at: from)
            photos.insert(moved, at: to)
        }
        Task { await model.move(in: title, from: from, to: to) }
    }
}

/// A photo opened from an album: the usual actions plus "Remove from album".
private struct AlbumPhotoSheet: View {
    let model: AlbumModel
    let photo: AlbumModel.Photo
    let album: String?
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        PhotoSheet(model: model, photo: photo)
            .safeAreaInset(edge: .bottom) {
                if let album {
                    Button("Remove from \(album)") {
                        Task {
                            await model.remove(photo.name, from: album)
                            dismiss()
                        }
                    }
                    .buttonStyle(.quiet(DottyLight.ember.color))
                    .padding(.bottom, Spacing.l)
                }
            }
    }
}

/// Pick photos already on Dotty to add to an album.
private struct ChooseFromDotty: View {
    let model: AlbumModel
    let excluded: Set<String>
    var onAdd: ([String]) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var selection: Set<String>? = []

    private var available: [String] { model.photos.map(\.name).filter { !excluded.contains($0) } }

    var body: some View {
        VStack(alignment: .leading, spacing: Spacing.l) {
            HStack {
                Text("Add photos from Dotty").font(.lpTitle).foregroundStyle(Color.ink)
                Spacer()
                Button("Cancel") { dismiss() }.buttonStyle(.quiet())
            }
            if available.isEmpty {
                Text("Every photo on Dotty is already in this album.").font(.lpCallout).foregroundStyle(Color.inkMuted)
                Spacer()
            } else {
                ScrollView {
                    PhotoGrid(model: model, names: available, selection: $selection, onOpen: { _ in })
                }
                Button(addTitle) {
                    let names = available.filter { selection?.contains($0) == true }
                    onAdd(names)
                    dismiss()
                }
                .buttonStyle(.light())
                .disabled(selection?.isEmpty != false)
                .frame(maxWidth: .infinity)
            }
        }
        .padding(Spacing.xl)
        .glassSurface(cornerRadius: Radius.sheet, frosted: true)
        .padding(Spacing.s)
    }

    private var addTitle: String {
        let count = selection?.count ?? 0
        return count == 1 ? "Add 1 photo" : "Add \(count) photos"
    }
}
