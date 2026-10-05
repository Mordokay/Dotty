import PhotosUI
import SwiftUI

/// The Album Viewer cartridge's screen: what Dotty shows, every photo on Dotty, and albums.
struct AlbumView: View {
    @Environment(DottyLink.self) private var link
    @State private var model: AlbumModel?

    var body: some View {
        Group {
            if let model {
                AlbumContent(model: model)
            } else {
                LightField { Color.clear }
            }
        }
        .onAppear { if model == nil { model = AlbumModel(link: link) } }
    }
}

enum AlbumPage: Hashable, CaseIterable {
    case photos, albums

    var title: String {
        switch self {
        case .photos: "Photos"
        case .albums: "Albums"
        }
    }
}

/// Picked photos on their way through the editor, and the album they go into.
struct EditorRequest: Identifiable {
    let id = UUID()
    let items: [PhotosPickerItem]
    let album: String?
}

private struct AlbumContent: View {
    @Bindable var model: AlbumModel
    @Environment(DottyLink.self) private var link
    @State private var page = AlbumPage.photos
    /// Photos picked for a bulk action; nil when not selecting.
    @State private var selection: Set<String>?
    @State private var picking = false
    @State private var picked: [PhotosPickerItem] = []
    @State private var editor: EditorRequest?
    @State private var detail: AlbumModel.Photo?

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Album Viewer")
                        .padding(.top, Spacing.l)
                    LightTabs(tabs: AlbumPage.allCases, selection: $page) { $0.title }
                    if !model.outbox.syncing { NotConnectedNotice() }
                    if let notice = model.notice { NoticeCard(kind: .success, text: notice) }
                    if let error = model.error { NoticeCard(kind: .error, text: error) }
                    SendingCard(outbox: model.outbox)
                    if !model.loaded && model.error == nil {
                        if link.connection == .connected {
                            HStack { Spacer(); FireflyLoader(size: 96, label: "Reading Dotty's photos"); Spacer() }
                                .padding(.top, Spacing.xxl)
                        }
                    } else {
                        Group {
                            switch page {
                            case .photos:
                                OnDottyCard(model: model)
                                PhotosCard(model: model, selection: $selection, picking: $picking, detail: $detail)
                            case .albums:
                                AlbumsCard(model: model)
                            }
                        }
                        .disabled(link.connection != .connected && !model.outbox.syncing)
                        .opacity(link.connection == .connected || model.outbox.syncing ? 1 : 0.45)
                        .animation(.settle, value: link.connection)
                    }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await model.load() }
            .photoSelectionBar(model: model, selection: $selection)
        }
        .navigationTitle("")
        .navigationDestination(for: AlbumModel.Album.self) { album in
            AlbumDetailView(model: model, name: album.name)
        }
        .photosPicker(isPresented: $picking, selection: $picked, maxSelectionCount: 30, matching: .images)
        .onChange(of: picked) { _, items in
            guard !items.isEmpty else { return }
            editor = EditorRequest(items: items, album: nil)
            picked = []
        }
        .photoEditorSheet(model: model, request: $editor)
        .sheet(item: $detail) { photo in
            PhotoSheet(model: model, photo: photo)
                .presentationDetents([.large])
                .presentationBackground(.clear)
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

extension View {
    /// The editor for picked photos; when it closes, the new photos are sent.
    func photoEditorSheet(model: AlbumModel, request: Binding<EditorRequest?>) -> some View {
        sheet(item: request) { editing in
            PhotoEditor(items: editing.items, album: editing.album, model: model) { added in
                request.wrappedValue = nil
                if added > 0 { Task { await model.outbox.sync() } }
            }
            .presentationDetents([.large])
            .presentationBackground(.clear)
            .interactiveDismissDisabled()
        }
    }
}

// MARK: - Cards

/// The photo on Dotty's screen, and what its lock screen shows.
private struct OnDottyCard: View {
    let model: AlbumModel
    @Environment(DottyLink.self) private var link

    var body: some View {
        let now = model.onDotty
        GlassCard(title: "On Dotty") {
            HStack(spacing: Spacing.m) {
                EpaperImage(image: now.photo.isEmpty ? nil : model.picture(now.photo))
                    .frame(width: 72, height: 72)
                    .clipShape(RoundedRectangle(cornerRadius: 6))
                VStack(alignment: .leading, spacing: 2) {
                    Text(now.album.isEmpty ? "All photos" : now.album).font(.lpHeadline).foregroundStyle(Color.ink)
                    Text(now.count == 0 ? "No photos yet" : "Photo \(now.index + 1) of \(now.count)")
                        .font(.lpCaption).foregroundStyle(Color.inkMuted)
                    Label(now.lockPhoto == nil ? "Lock screen: this album, a new photo every \(now.everyText)"
                                               : "Lock screen: always the same photo",
                          systemImage: "lock")
                        .font(.lpCaption).foregroundStyle(Color.inkMuted)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 0)
            }
            .padding(Spacing.m)
            if link.info?.isAtLeast("0.2.0") == true { SlideshowIntervalRow(model: model) }
        }
    }
}

/// How often the lock-screen slideshow changes photo. Each change is a full refresh of the
/// e-paper (what wears it, and costs battery), so the choices start at 10 minutes.
struct SlideshowIntervalRow: View {
    let model: AlbumModel

    var body: some View {
        let every = model.onDotty.every
        HStack(spacing: Spacing.m) {
            VStack(alignment: .leading, spacing: 2) {
                Text("Slideshow").font(.lpHeadline).foregroundStyle(Color.ink)
                Text("A new photo on the lock screen every…")
                    .font(.lpCaption).foregroundStyle(Color.inkMuted)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: Spacing.s)
            MorphButton(faces: AlbumModel.intervals.map { minutes in
                            MorphFace(light: minutes == 10 ? DottyLight.amber.color : minutes == 30 ? DottyLight.lagoon.color
                                                                                                    : DottyLight.dusk.color,
                                      title: AlbumModel.interval(minutes), systemImage: "timer")
                        },
                        initial: AlbumModel.intervals.firstIndex(of: every) ?? 1) { index in
                Task { await model.setSlideshow(every: AlbumModel.intervals[index]) }
            }
            .fixedSize()
            .id(every)  // follow Dotty when it changes elsewhere
        }
        .padding(Spacing.m)
    }
}

/// Photos waiting to go, or going, to Dotty over Wi-Fi.
private struct SendingCard: View {
    let outbox: PhotoOutbox

    var body: some View {
        if let notice = outbox.notice { NoticeCard(kind: .success, text: notice) }
        if let error = outbox.error { NoticeCard(kind: .error, text: error) }
        if !outbox.uploads.isEmpty {
            GlassCard(title: "Sending photos") {
                VStack(alignment: .leading, spacing: Spacing.s) {
                    if outbox.syncing {
                        LightProgress(value: Double(outbox.sent) / Double(max(1, outbox.uploads.count)))
                        Text(outbox.stage ?? "").font(.lpCaption).foregroundStyle(Color.inkMuted)
                        Text("Keep this app open until it's done; your screen stays on.")
                            .font(.lpCaption).foregroundStyle(Color.inkFaint)
                    } else {
                        Text(outbox.uploads.count == 1 ? "1 photo waiting" : "\(outbox.uploads.count) photos waiting")
                            .font(.lpCallout).foregroundStyle(Color.ink)
                        HStack(spacing: Spacing.m) {
                            Button("Send now") { Task { await outbox.sync() } }.buttonStyle(.light())
                            Button("Discard") { outbox.discardAll() }.buttonStyle(.quiet(DottyLight.ember.color))
                        }
                    }
                }
                .padding(Spacing.m)
            }
        }
    }
}

/// Every photo on Dotty, in date order, with "Add photos" and multi-select.
private struct PhotosCard: View {
    let model: AlbumModel
    @Binding var selection: Set<String>?
    @Binding var picking: Bool
    @Binding var detail: AlbumModel.Photo?

    var body: some View {
        GlassCard {
            HStack(spacing: Spacing.m) {
                Text(model.photos.count == 1 ? "1 photo" : "\(model.photos.count) photos")
                    .font(.lpTitle).foregroundStyle(Color.ink)
                Spacer()
                if !model.photos.isEmpty {
                    Button(selection == nil ? "Select" : "Done") {
                        withAnimation(.settle) { selection = selection == nil ? [] : nil }
                    }
                    .buttonStyle(.quiet())
                }
            }
            .padding(Spacing.m)
            if selection == nil {
                LightRow(title: "Add photos", subtitle: "From your iPhone; you frame each one",
                         systemImage: "plus", action: { picking = true })
            }
            PhotoGrid(model: model, names: model.photos.map(\.name), selection: $selection) { name in
                detail = model.photos.first { $0.name == name }
            }
            .padding(Spacing.s)
        }
    }
}

/// Square e-paper thumbnails, three a row. Selecting: tap toggles; otherwise tap opens.
struct PhotoGrid: View {
    let model: AlbumModel
    let names: [String]
    @Binding var selection: Set<String>?
    var onOpen: (String) -> Void
    /// Album order editing: drop one photo on another to move it there.
    var onMove: ((_ from: Int, _ to: Int) -> Void)?

    private let columns = Array(repeating: GridItem(.flexible(), spacing: 6), count: 3)

    var body: some View {
        LazyVGrid(columns: columns, spacing: 6) {
            ForEach(Array(names.enumerated()), id: \.element) { index, name in
                cell(name)
                    .onTapGesture {
                        if var picked = selection {
                            if picked.contains(name) { picked.remove(name) } else { picked.insert(name) }
                            selection = picked
                        } else {
                            onOpen(name)
                        }
                    }
                    .modifier(Reorder(name: name, index: index, names: names, onMove: onMove))
            }
        }
    }

    private func cell(_ name: String) -> some View {
        let picked = selection?.contains(name) == true
        let onScreen = model.onDotty.photo == name
        return EpaperImage(image: model.picture(name))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .overlay {
                RoundedRectangle(cornerRadius: 6)
                    .strokeBorder(picked ? DottyLight.firefly.color : onScreen ? Color.ink.opacity(0.7) : .clear,
                                  lineWidth: picked ? 3 : 1.5)
            }
            .overlay(alignment: .topTrailing) {
                if selection != nil {
                    Image(systemName: picked ? "checkmark.circle.fill" : "circle")
                        .font(.system(size: 20, weight: .semibold))
                        .foregroundStyle(picked ? DottyLight.firefly.color : .white, .black.opacity(0.4))
                        .padding(5)
                }
            }
            .contentShape(Rectangle())
            .accessibilityLabel(AlbumModel.dateTaken(name)?.formatted(date: .long, time: .omitted) ?? name)
            .accessibilityAddTraits(picked ? .isSelected : [])
    }

    /// Drag a photo onto another to move it to that place (albums only).
    private struct Reorder: ViewModifier {
        let name: String
        let index: Int
        let names: [String]
        let onMove: ((Int, Int) -> Void)?

        func body(content: Content) -> some View {
            if let onMove {
                content
                    .draggable(name)
                    .dropDestination(for: String.self) { dropped, _ in
                        guard let moved = dropped.first, let from = names.firstIndex(of: moved), from != index else { return false }
                        onMove(from, index)
                        return true
                    }
            } else {
                content
            }
        }
    }
}

/// "All photos" and every album, each opening its own page.
private struct AlbumsCard: View {
    let model: AlbumModel
    @State private var naming = false
    @State private var newName = ""
    @State private var renaming: String?
    @State private var deleting: String?

    var body: some View {
        GlassCard {
            NavigationLink(value: AlbumModel.Album(name: "", count: model.photos.count, cover: model.photos.first?.name)) {
                AlbumRow(model: model, title: "All photos", count: model.photos.count, cover: model.photos.first?.name,
                         active: model.onDotty.album.isEmpty)
            }
            .buttonStyle(.plain)
            ForEach(model.albums) { album in
                NavigationLink(value: album) {
                    AlbumRow(model: model, title: album.name, count: album.count, cover: album.cover,
                             active: model.onDotty.album == album.name)
                }
                .buttonStyle(.plain)
                .contextMenu {
                    Button("Show on Dotty", systemImage: "photo") { Task { await model.show(album: album.name) } }
                    Button("Rename", systemImage: "pencil") { renaming = album.name; newName = album.name }
                    Button("Delete album", systemImage: "trash", role: .destructive) { deleting = album.name }
                }
            }
            LightRow(title: "New album", systemImage: "plus", action: { naming = true })
        }
        .alert("New album", isPresented: $naming) {
            TextField("Name", text: $newName)
            Button("Create") {
                let name = newName.trimmingCharacters(in: .whitespaces)
                newName = ""
                if !name.isEmpty { Task { await model.createAlbum(name) } }
            }
            Button("Cancel", role: .cancel) { newName = "" }
        }
        .alert("Rename album", isPresented: .init(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            TextField("Name", text: $newName)
            Button("Rename") {
                let name = newName.trimmingCharacters(in: .whitespaces)
                if let old = renaming, !name.isEmpty, name != old { Task { _ = await model.renameAlbum(old, to: name) } }
                renaming = nil
            }
            Button("Cancel", role: .cancel) { renaming = nil }
        }
        .deleteAlbumDialog(model: model, name: $deleting)
    }
}

/// An album with its cover; the one Dotty shows has a small light.
private struct AlbumRow: View {
    let model: AlbumModel
    let title: String
    let count: Int
    let cover: String?
    let active: Bool

    var body: some View {
        HStack(spacing: Spacing.m) {
            EpaperImage(image: cover.flatMap { model.picture($0) })
                .frame(width: 48, height: 48)
                .clipShape(RoundedRectangle(cornerRadius: 6))
            VStack(alignment: .leading, spacing: 0) {
                Text(title).font(.lpHeadline).foregroundStyle(Color.ink)
                Text(count == 1 ? "1 photo" : "\(count) photos").font(.lpCaption).foregroundStyle(Color.inkMuted)
            }
            Spacer(minLength: Spacing.s)
            if active {
                Text("On Dotty").font(.lpLabel).foregroundStyle(DottyLight.firefly.color)
            }
            Image(systemName: "chevron.right")
                .font(.system(size: 14, weight: .semibold, design: .rounded))
                .foregroundStyle(Color.inkMuted)
        }
        .padding(.horizontal, Spacing.m)
        .padding(.vertical, Spacing.s)
        .contentShape(Rectangle())
    }
}

extension View {
    /// "Delete album?" — the photos stay on Dotty.
    func deleteAlbumDialog(model: AlbumModel, name: Binding<String?>, onDeleted: (() -> Void)? = nil) -> some View {
        confirmationDialog("Delete this album?", isPresented: .init(get: { name.wrappedValue != nil },
                                                                    set: { if !$0 { name.wrappedValue = nil } }),
                           titleVisibility: .visible, presenting: name.wrappedValue) { album in
            Button("Delete \(album)", role: .destructive) {
                Task {
                    await model.deleteAlbum(album)
                    onDeleted?()
                }
            }
        } message: { _ in
            Text("Only the album goes. Its photos stay on Dotty, in All photos.")
        }
    }
}

// MARK: - Selection bar

extension View {
    func photoSelectionBar(model: AlbumModel, selection: Binding<Set<String>?>) -> some View {
        modifier(PhotoSelectionBar(model: model, selection: selection))
    }
}

private struct PhotoSelectionBar: ViewModifier {
    let model: AlbumModel
    @Binding var selection: Set<String>?
    @State private var naming = false
    @State private var newAlbum = ""
    @State private var confirmDelete = false

    private var picked: [String] {
        model.photos.map(\.name).filter { selection?.contains($0) == true }
    }

    func body(content: Content) -> some View {
        content
            .safeAreaInset(edge: .bottom) {
                if let selected = selection {
                    HStack(spacing: Spacing.m) {
                        Text(selected.isEmpty ? "Pick photos" : "\(selected.count) picked")
                            .font(.lpCallout).foregroundStyle(Color.ink)
                        Spacer()
                        Menu {
                            ForEach(model.albums) { album in
                                Button(album.name) { finish { await model.add(picked, to: album.name) } }
                            }
                            Button("New album…", systemImage: "plus") { naming = true }
                        } label: {
                            Label("Add to album", systemImage: "rectangle.stack.badge.plus")
                        }
                        .buttonStyle(.light())
                        Button { confirmDelete = true } label: { Image(systemName: "trash") }
                            .buttonStyle(.quiet(DottyLight.ember.color))
                    }
                    .disabled(selected.isEmpty)
                    .padding(Spacing.m)
                    .glassSurface(cornerRadius: Radius.soft, frosted: true)
                    .padding(.horizontal, Spacing.l)
                    .padding(.bottom, Spacing.s)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
                }
            }
            .alert("New album", isPresented: $naming) {
                TextField("Name", text: $newAlbum)
                Button("Create") {
                    let name = newAlbum.trimmingCharacters(in: .whitespaces)
                    newAlbum = ""
                    let photos = picked
                    if !name.isEmpty { finish { await model.createAlbum(name, with: photos) } }
                }
                Button("Cancel", role: .cancel) { newAlbum = "" }
            }
            .confirmationDialog("Delete \(picked.count == 1 ? "this photo" : "\(picked.count) photos") from Dotty?",
                                isPresented: $confirmDelete, titleVisibility: .visible) {
                Button("Delete", role: .destructive) {
                    let photos = picked
                    finish { await model.deletePhotos(photos) }
                }
            } message: {
                Text("They're removed from the SD card and from every album.")
            }
    }

    private func finish(_ action: @escaping () async -> Void) {
        withAnimation(.settle) { selection = nil }
        Task { await action() }
    }
}

// MARK: - One photo

/// A photo large, with what can be done with it.
struct PhotoSheet: View {
    let model: AlbumModel
    let photo: AlbumModel.Photo
    @Environment(\.dismiss) private var dismiss
    @State private var naming = false
    @State private var newAlbum = ""
    @State private var confirmDelete = false

    var body: some View {
        VStack(alignment: .leading, spacing: Spacing.l) {
            HStack {
                Text(photo.taken?.formatted(date: .long, time: .shortened) ?? "Photo")
                    .font(.lpTitle).foregroundStyle(Color.ink)
                Spacer()
                Button("Done") { dismiss() }.buttonStyle(.quiet())
            }
            EpaperImage(image: model.picture(photo.name))
                .clipShape(RoundedRectangle(cornerRadius: 8))
            VStack(spacing: 0) {
                LightRow(title: "Show on Dotty", systemImage: "photo") {
                    Button("Show") { Task { await model.show(album: model.onDotty.album, photo: photo.name) } }
                        .buttonStyle(.quiet())
                }
                ChoiceRow(title: "Keep on the lock screen", subtitle: "Instead of the album's slideshow",
                          systemImage: "lock", chosen: model.onDotty.lockPhoto == photo.name) {
                    Task {
                        if model.onDotty.lockPhoto == photo.name {
                            await model.lockScreen(album: model.onDotty.album)
                        } else {
                            await model.lockScreen(photo: photo.name)
                        }
                    }
                }
                LightRow(title: "Add to album", systemImage: "rectangle.stack.badge.plus") {
                    Menu("Choose") {
                        ForEach(model.albums) { album in
                            Button(album.name) { Task { await model.add([photo.name], to: album.name) } }
                        }
                        Button("New album…", systemImage: "plus") { naming = true }
                    }
                    .buttonStyle(.quiet())
                }
                LightRow(title: "Delete from Dotty", systemImage: "trash") {
                    Button("Delete") { confirmDelete = true }.buttonStyle(.quiet(DottyLight.ember.color))
                }
            }
            .glassSurface(cornerRadius: Radius.soft)
            Spacer(minLength: 0)
        }
        .padding(Spacing.xl)
        .glassSurface(cornerRadius: Radius.sheet, frosted: true)
        .padding(Spacing.s)
        .alert("New album", isPresented: $naming) {
            TextField("Name", text: $newAlbum)
            Button("Create") {
                let name = newAlbum.trimmingCharacters(in: .whitespaces)
                newAlbum = ""
                if !name.isEmpty { Task { await model.createAlbum(name, with: [photo.name]) } }
            }
            Button("Cancel", role: .cancel) { newAlbum = "" }
        }
        .confirmationDialog("Delete this photo from Dotty?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                Task {
                    await model.deletePhotos([photo.name])
                    dismiss()
                }
            }
        } message: {
            Text("It's removed from the SD card and from every album.")
        }
    }
}
