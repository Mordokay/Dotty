import PhotosUI
import SwiftUI

/// The companions (firmware: cartridges/companion/): a pager, one page per companion — each its own
/// person with a name (given when it first wakes up on Dotty), character, voice, avatar, memories
/// and chats — and a "+" page while there are fewer than 8. Settling on a page makes that companion
/// the one on Dotty. Shared settings (keys, voice on/off, volume) sit behind the gear.
struct CompanionView: View {
    @Environment(DottyLink.self) private var link
    @State private var companions: [Summary] = []
    @State private var active = ""
    @State private var max = 8
    @State private var favourites: [String] = []
    @State private var selection = ""
    @State private var selectTask: Task<Void, Never>?
    @State private var showSettings = false
    @State private var error: String?

    struct Summary: Identifiable, Hashable {
        let id: String, type: String, name: String, avatar: String, voice: String
        let awake: Bool, memories: Int
    }

    static let newPage = "+"

    var body: some View {
        LightField {
            VStack(spacing: Spacing.s) {
                HStack {
                    Text("Companions").font(.lpTitle).foregroundStyle(Color.ink)
                    Spacer()
                    NavigationLink { PlacesView() } label: {
                        Image(systemName: "map").font(.system(size: 20, weight: .semibold))
                            .foregroundStyle(Color.ink)
                    }
                    .accessibilityLabel("Places")
                    .padding(.trailing, Spacing.s)
                    NavigationLink { MomentsView() } label: {
                        Image(systemName: "clock").font(.system(size: 20, weight: .semibold))
                            .foregroundStyle(Color.ink)
                    }
                    .accessibilityLabel("Moments")
                    .padding(.trailing, Spacing.s)
                    NavigationLink { MacrosView() } label: {
                        Image(systemName: "wand.and.stars").font(.system(size: 20, weight: .semibold))
                            .foregroundStyle(Color.ink)
                    }
                    .accessibilityLabel("Macros")
                    .padding(.trailing, Spacing.s)
                    Button { showSettings = true } label: {
                        Image(systemName: "gearshape").font(.system(size: 20, weight: .semibold))
                            .foregroundStyle(Color.ink)
                    }
                    .accessibilityLabel("Companion settings")
                }
                .padding(.horizontal, Spacing.l)
                .padding(.top, Spacing.s)
                NotConnectedNotice().padding(.horizontal, Spacing.l)
                if let error { NoticeCard(kind: .error, text: error).padding(.horizontal, Spacing.l) }
                TabView(selection: $selection) {
                    ForEach(companions) { c in
                        CompanionPage(summary: c, isActive: c.id == active, favourites: favourites,
                                      canDelete: companions.count > 1, onChanged: { Task { await load() } })
                            .tag(c.id)
                    }
                    if companions.count < max {
                        NewCompanionPage(onCreated: { id in
                            Task {
                                await load()
                                selection = id
                            }
                        })
                        .tag(Self.newPage)
                    }
                }
                .tabViewStyle(.page(indexDisplayMode: .never))
                CompanionPager(companions: companions, selection: $selection, active: active,
                               canAdd: companions.count < max, newPage: Self.newPage)
                    .padding(.bottom, Spacing.s)
            }
        }
        .navigationTitle("")
        .task { await load() }
        .onChange(of: link.connection) { _, state in
            if state == .connected { Task { await load() } }
        }
        .onChange(of: link.eventCount) { _, _ in
            if link.lastEvent?.event == "companion.changed" { Task { await load() } }
        }
        // Settling on a page for half a second makes that companion the one on Dotty.
        .onChange(of: selection) { _, id in
            selectTask?.cancel()
            guard id != Self.newPage, id != active, !id.isEmpty else { return }
            selectTask = Task {
                try? await Task.sleep(for: .milliseconds(500))
                guard !Task.isCancelled, link.connection == .connected else { return }
                do {
                    try await link.send("companion.select", ["id": id])
                    active = id
                } catch {
                    if !link.lostConnection(error) { self.error = error.localizedDescription }
                }
            }
        }
        .sheet(isPresented: $showSettings) { CompanionSettingsSheet() }
    }

    private func load() async {
        do {
            let r = try await link.send("companion.list")
            companions = (r["companions"] as? [[String: Any]] ?? []).map { c in
                Summary(id: c["id"] as? String ?? "", type: c["type"] as? String ?? "custom", name: c["name"] as? String ?? "",
                        avatar: c["avatar"] as? String ?? "robot", voice: c["voice"] as? String ?? "Zephyr",
                        awake: c["awake"] as? Bool ?? false, memories: c["memories"] as? Int ?? 0)
            }
            active = r["active"] as? String ?? ""
            max = r["max"] as? Int ?? 8
            if selection.isEmpty || !(companions.contains { $0.id == selection } || selection == Self.newPage) {
                selection = active
            }
            let status = try await link.send("companion.status")
            favourites = status["favourites"] as? [String] ?? []
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    /// The avatar picture: one of the ten (Resources/Avatars), or the companion's photo (kept on
    /// this iPhone when it was uploaded).
    @ViewBuilder static func avatarImage(_ avatar: String, companion: String, size: CGFloat) -> some View {
        Group {
            if avatar == "photo", let photo = AvatarPhotos.load(companion) {
                Image(uiImage: photo).interpolation(.none).resizable().scaledToFit().clipShape(Circle())
            } else if let image = UIImage(named: avatar == "photo" ? "robot" : avatar) {
                Image(uiImage: image).interpolation(.none).resizable().scaledToFit()
            } else {
                Image(systemName: "person.crop.circle").resizable().scaledToFit().foregroundStyle(Color.inkMuted)
            }
        }
        .frame(width: size, height: size)
    }

    static func typeName(_ type: String) -> String {
        ["coach": "Coach", "friend": "Friend", "partner": "Partner", "teacher": "Teacher", "debug": "Debug"][type] ?? "Custom"
    }
}

/// One companion: who they are and what they know.
struct CompanionPage: View {
    let summary: CompanionView.Summary
    let isActive: Bool
    let favourites: [String]
    let canDelete: Bool
    let onChanged: () -> Void
    @Environment(DottyLink.self) private var link
    @State private var profile = Profile()
    @State private var saved = Profile()
    @State private var loaded = false
    @State private var saving = false
    @State private var confirmDelete = false
    @State private var error: String?
    @State private var photoItem: PhotosPickerItem?
    @State private var pickingPhoto = false
    @State private var library: [String] = []
    @State private var uploading: Double?
    @State private var photoToEdit: PickedPhoto?

    struct PickedPhoto: Identifiable {
        let id = UUID()
        let image: CGImage
    }

    /// The character fields, each edited line by line; a pinned line never changes by itself
    /// (Dotty puts it back when the companion evolves).
    static let characterFields = ["personality", "traits", "quirks", "speaking", "backstory"]

    struct Line: Identifiable, Equatable {
        var id = UUID()
        var text: String
        var pinned = false
    }

    struct Profile: Equatable {
        var name = "", voice = "Zephyr", avatar = "robot"
        var lines: [String: [Line]] = [:]

        /// What Dotty stores: the non-empty lines, one per line.
        func text(_ field: String) -> String {
            (lines[field] ?? []).map { $0.text.trimmingCharacters(in: .whitespacesAndNewlines) }
                .filter { !$0.isEmpty }.joined(separator: "\n")
        }

        /// The pinned lines as numbers of the lines in text(field).
        func pins(_ field: String) -> [Int] {
            let kept = (lines[field] ?? []).filter { !$0.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
            return kept.indices.filter { kept[$0].pinned }
        }
    }

    /// Dotty's lines, prose split into sentences so each can be pinned (a pinned line pins all of its sentences).
    static func lines(_ text: String, prose: Bool, pinned: Set<Int>) -> [Line] {
        let dotty = text.split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        return dotty.enumerated().flatMap { i, line in
            (prose ? sentences(line) : [line]).map { Line(text: $0, pinned: pinned.contains(i)) }
        }
    }

    /// "One thing. Another!" → two sentences: a split after . ! ? followed by a space and a capital.
    static func sentences(_ text: String) -> [String] {
        var out: [String] = [], current = ""
        let chars = Array(text)
        for (i, ch) in chars.enumerated() {
            current.append(ch)
            if ".!?".contains(ch), i + 2 < chars.count, chars[i + 1] == " ", chars[i + 2].isUppercase {
                out.append(current.trimmingCharacters(in: .whitespaces))
                current = ""
            }
        }
        if !current.trimmingCharacters(in: .whitespaces).isEmpty { out.append(current.trimmingCharacters(in: .whitespaces)) }
        return out
    }

    static let avatars: [(id: String, name: String)] = [
        ("robot", "Robot"), ("dog", "Dog"), ("cat", "Cat"), ("fox", "Fox"), ("owl", "Owl"),
        ("bear", "Bear"), ("girl", "Girl"), ("guy", "Guy"), ("ghost", "Ghost"), ("dragon", "Dragon"),
    ]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Spacing.xl) {
                header
                if let error { NoticeCard(kind: .error, text: error) }
                if isActive { NavigationCard() }  // a trip under way (directions)
                if isActive && summary.awake { LastLineCard(name: displayName) }
                avatarCard.needsDotty(link)
                voiceCard.needsDotty(link)
                characterCard.needsDotty(link)
                GlassCard(title: "What they know") {
                    NavigationLink { MemoriesView(companion: summary.id) } label: {
                        LightRow(title: summary.memories == 1 ? "1 memory" : "\(summary.memories) memories",
                                 subtitle: "Things \(displayName) remembers about you", systemImage: "bookmark") { chevron }
                    }
                    .buttonStyle(.plain)
                    NavigationLink { CompanionDayView(day: CompanionHistoryView.today, companion: summary.id) } label: {
                        LightRow(title: "Today's chat", subtitle: "Updates a moment after each chat on Dotty",
                                 systemImage: "bubble.left.and.bubble.right") { chevron }
                    }
                    .buttonStyle(.plain)
                    NavigationLink { RecordingsView(companion: summary.id, name: displayName) } label: {
                        LightRow(title: "Recordings", subtitle: "Voice notes you recorded with \(displayName)",
                                 systemImage: "waveform") { chevron }
                    }
                    .buttonStyle(.plain)
                    NavigationLink { CompanionHistoryView(companion: summary.id) } label: {
                        LightRow(title: "Past chats", subtitle: "What you said and what \(displayName) answered",
                                 systemImage: "clock.arrow.circlepath") { chevron }
                    }
                    .buttonStyle(.plain)
                    NavigationLink {
                        EvolutionView(companion: summary.id, name: displayName) { Task { await load() } }
                    } label: {
                        LightRow(title: "How \(displayName) has changed", subtitle: "Small changes after chats, with undo",
                                 systemImage: "leaf") { chevron }
                    }
                    .buttonStyle(.plain)
                }
                .needsDotty(link)
                if canDelete {
                    Button("Delete \(displayName)", role: .destructive) { confirmDelete = true }
                        .buttonStyle(.quiet())
                        .frame(maxWidth: .infinity)
                        .needsDotty(link)
                }
            }
            .padding(.horizontal, Spacing.l)
            .padding(.bottom, Spacing.xxl * 2)
        }
        .keyboardDismissing()
        .task(id: summary) { await load() }
        .confirmationDialog("Delete \(displayName)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) { Task { await delete() } }
        } message: {
            Text("Their memories and chats go too.")
        }
    }

    private var displayName: String { summary.awake ? summary.name : "this companion" }
    private var chevron: some View {
        Image(systemName: "chevron.right").font(.system(size: 14, weight: .semibold, design: .rounded))
    }

    private var header: some View {
        HStack(spacing: Spacing.m) {
            CompanionView.avatarImage(profile.avatar, companion: summary.id, size: 88)
                .padding(6)
                .background(RoundedRectangle(cornerRadius: 16).fill(Color.white))
            VStack(alignment: .leading, spacing: 4) {
                Text(summary.awake ? summary.name : "Not awake yet").font(.lpTitle).foregroundStyle(Color.ink)
                Text(CompanionView.typeName(summary.type)).font(.lpCallout).foregroundStyle(Color.inkMuted)
                if isActive {
                    Text("On Dotty").font(.lpLabel).foregroundStyle(DottyLight.firefly.color)
                } else if !summary.awake {
                    Text("Talk to them on Dotty to wake them up").font(.lpCaption).foregroundStyle(Color.inkFaint)
                }
            }
            Spacer()
        }
        .padding(.top, Spacing.m)
    }

    private var avatarCard: some View {
        GlassCard(title: "Avatar") {
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: Spacing.s), count: 5), spacing: Spacing.s) {
                ForEach(Self.avatars, id: \.id) { a in
                    Button { Task { await update(["avatar": a.id]) } } label: {
                        VStack(spacing: 2) {
                            CompanionView.avatarImage(a.id, companion: summary.id, size: 48)
                                .padding(4)
                                .background(RoundedRectangle(cornerRadius: 12).fill(Color.white))
                                .overlay(RoundedRectangle(cornerRadius: 12)
                                    .strokeBorder(a.id == profile.avatar ? DottyLight.firefly.color : Color.glassEdge,
                                                  lineWidth: a.id == profile.avatar ? 3 : 1))
                            Text(a.name).font(.lpCaption).foregroundStyle(a.id == profile.avatar ? Color.ink : Color.inkMuted)
                        }
                    }
                    .buttonStyle(.plain)
                }
                ForEach(library, id: \.self) { photo in
                    Button { Task { await wear(photo) } } label: {
                        VStack(spacing: 2) {
                            Group {
                                if let image = AvatarPhotos.preview(photo) {
                                    Image(uiImage: image).interpolation(.none).resizable().scaledToFit().clipShape(Circle())
                                } else {
                                    Image(systemName: "photo").resizable().scaledToFit()
                                }
                            }
                            .frame(width: 48, height: 48)
                            .padding(4)
                            .background(RoundedRectangle(cornerRadius: 12).fill(Color.white))
                            .overlay(RoundedRectangle(cornerRadius: 12)
                                .strokeBorder(wearing(photo) ? DottyLight.firefly.color : Color.glassEdge, lineWidth: wearing(photo) ? 3 : 1))
                            Text("Photo").font(.lpCaption).foregroundStyle(wearing(photo) ? Color.ink : Color.inkMuted)
                        }
                    }
                    .buttonStyle(.plain)
                    .contextMenu {
                        Button("Delete photo", systemImage: "trash", role: .destructive) {
                            AvatarPhotos.delete(photo)
                            library = AvatarPhotos.all()
                        }
                    }
                }
                Button { pickingPhoto = true } label: {
                    VStack(spacing: 2) {
                        Image(systemName: "photo.badge.plus").font(.title2).foregroundStyle(Color.ink)
                            .frame(width: 48, height: 48)
                            .padding(4)
                            .background(RoundedRectangle(cornerRadius: 12).fill(Color.white))
                            .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Color.glassEdge, lineWidth: 1))
                        Text("Add photo").font(.lpCaption).foregroundStyle(Color.inkMuted)
                    }
                }
                .buttonStyle(.plain)
            }
            .padding(Spacing.m)
            if let uploading {
                ProgressView(value: uploading) { Text("Sending the photo to Dotty…").font(.lpCaption) }
                    .padding(.horizontal, Spacing.m)
            }
            Text("Your photos are kept on this iPhone for every companion. Long-press one to delete it.")
                .font(.lpCaption).foregroundStyle(Color.inkFaint)
                .padding(.horizontal, Spacing.m).padding(.bottom, Spacing.s)
        }
        .onAppear { library = AvatarPhotos.all() }
        .photosPicker(isPresented: $pickingPhoto, selection: $photoItem, matching: .images)
        .onChange(of: photoItem) { _, item in
            guard let item else { return }
            Task {
                if let data = try? await item.loadTransferable(type: Data.self), let picked = UIImage(data: data),
                   let upright = PhotoDither.upright(picked) {
                    photoToEdit = PickedPhoto(image: upright)
                }
                photoItem = nil
            }
        }
        .sheet(item: $photoToEdit) { photo in
            AvatarPhotoEditor(companion: summary.id, image: photo.image) { _ in
                library = AvatarPhotos.all()
                Task {
                    await load()
                    onChanged()
                }
            }
        }
    }

    private func wearing(_ photo: String) -> Bool {
        profile.avatar == "photo" && AvatarPhotos.photo(for: summary.id) == photo
    }

    /// A photo from the library onto this companion.
    private func wear(_ photo: String) async {
        guard !wearing(photo), let bitmaps = AvatarPhotos.bitmaps(photo) else { return }
        uploading = 0
        defer { uploading = nil }
        do {
            try await AvatarPhotos.send(link, companion: summary.id, big: bitmaps.big, small: bitmaps.small) { uploading = $0 }
            AvatarPhotos.setPhoto(photo, for: summary.id)
            await load()
            onChanged()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private var voiceCard: some View {
        GlassCard(title: "Voice") {
            NavigationLink {
                VoicesView(selected: profile.voice, favourites: favourites,
                           onPick: { name in Task { await update(["voice": name]) } },
                           onFavourites: { list in Task { try? await link.send("companion.settings", ["favourites": list]) } })
            } label: {
                LightRow(title: profile.voice, subtitle: "Stars mark the voices Dotty's settings step through",
                         systemImage: "waveform") { chevron }
            }
            .buttonStyle(.plain)
        }
    }

    private var characterCard: some View {
        GlassCard(title: "Who they are") {
            VStack(alignment: .leading, spacing: Spacing.m) {
                CharacterField(title: "Name", text: $profile.name, limit: 24, lines: 1,
                               hint: summary.awake ? nil : "Usually given when they first wake up on Dotty")
                LinesField(title: "Personality", lines: lines("personality"), list: false, limit: 600,
                           suggest: { await suggest("personality", "personality") })
                LinesField(title: "Traits", lines: lines("traits"), list: true, limit: 40,
                           suggest: { await suggest("traits", "set of traits") })
                LinesField(title: "Quirks", lines: lines("quirks"), list: true, limit: 60,
                           suggest: { await suggest("quirks", "set of quirks") })
                LinesField(title: "Way of speaking", lines: lines("speaking"), list: false, limit: 300,
                           suggest: { await suggest("speaking", "way of speaking") })
                LinesField(title: "Backstory", lines: lines("backstory"), list: false, limit: 600,
                           suggest: { await suggest("backstory", "backstory") })
                Text("\(Image(systemName: "pin.fill")) Pinned lines never change by themselves. The rest can change slowly as you talk.")
                    .font(.lpCaption).foregroundStyle(Color.inkFaint)
                if profile != saved {
                    HStack {
                        Button("Undo") { profile = saved }.buttonStyle(.quiet())
                        Spacer()
                        if saving { ProgressView().controlSize(.small) }
                        Button("Save") { Task { await save() } }.buttonStyle(.light()).disabled(saving)
                    }
                }
            }
            .padding(Spacing.m)
        }
    }

    private func lines(_ field: String) -> Binding<[Line]> {
        Binding(get: { profile.lines[field] ?? [] }, set: { profile.lines[field] = $0 })
    }

    /// ✨ Suggest: Gemini writes this field again, keeping the rest of the character in mind and the
    /// pinned lines as they are (put back at their place if it drops them anyway).
    private func suggest(_ field: String, _ label: String) async {
        var current: [String: String] = [:]
        for f in Self.characterFields { current[f] = profile.text(f) }
        let old = profile.lines[field] ?? []
        let keep = old.filter(\.pinned).map(\.text)
        let (prompt, schema) = CompanionDesign.field(field, label: label, description: "", current: current,
                                                     name: profile.name, voice: profile.voice, avatar: profile.avatar,
                                                     keep: keep)
        do {
            let answer = try await GeminiText.generate(prompt, schema: schema)
            let value = (answer["value"] as? [String])?.joined(separator: "\n") ?? answer["value"] as? String ?? ""
            var fresh = Self.lines(value, prose: field != "traits" && field != "quirks", pinned: [])
            for (i, line) in old.enumerated() where line.pinned {
                if let at = fresh.firstIndex(where: { $0.text == line.text }) {
                    fresh[at].pinned = true
                } else {
                    fresh.insert(line, at: min(i, fresh.count))
                }
            }
            profile.lines[field] = fresh
            error = nil
        } catch {
            self.error = error.localizedDescription
        }
    }

    // MARK: - Dotty

    private func load() async {
        do {
            let r = try await link.send("companion.get", ["id": summary.id])
            let c = r["companion"] as? [String: Any] ?? [:]
            var p = Profile()
            p.name = c["name"] as? String ?? ""
            let pins = c["pins"] as? [String: [Int]] ?? [:]
            for field in Self.characterFields {
                let text = (c[field] as? [String])?.joined(separator: "\n") ?? c[field] as? String ?? ""
                p.lines[field] = Self.lines(text, prose: !(c[field] is [String]), pinned: Set(pins[field] ?? []))
            }
            p.voice = c["voice"] as? String ?? "Zephyr"
            p.avatar = c["avatar"] as? String ?? "robot"
            profile = p
            saved = p
            loaded = true
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func update(_ fields: [String: Any]) async {
        do {
            try await link.send("companion.update", fields.merging(["id": summary.id]) { a, _ in a })
            await load()
            onChanged()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    /// Changed fields only; a long one goes in pieces (a command carries at most 512 bytes). A field's
    /// pins go after its text, as numbers of its lines (Dotty keeps their text).
    private func save() async {
        saving = true
        defer { saving = false }
        do {
            if profile.name != saved.name {
                try await CompanionPage.sendField(link, id: summary.id, field: "name", value: profile.name)
            }
            for field in Self.characterFields where profile.lines[field] != saved.lines[field] {
                try await CompanionPage.sendField(link, id: summary.id, field: field, value: profile.text(field))
                try await link.send("companion.pins", ["id": summary.id, "field": field, "lines": profile.pins(field)])
            }
            await load()
            onChanged()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    static func sendField(_ link: DottyLink, id: String, field: String, value: String) async throws {
        // 240 bytes a piece: JSON escapes (quotes, slashes, newlines) must still fit 512 per command.
        if value.utf8.count <= 240 {
            try await link.send("companion.update", ["id": id, field: value])
            return
        }
        var pieces: [String] = []
        var current = ""
        for ch in value {  // whole characters, ≤ 240 bytes a piece
            if (current + String(ch)).utf8.count > 240 {
                pieces.append(current)
                current = ""
            }
            current.append(ch)
        }
        pieces.append(current)
        var offset = 0
        let total = value.utf8.count
        for piece in pieces {
            try await link.send("companion.field", ["id": id, "field": field, "offset": offset, "total": total, "text": piece])
            offset += piece.utf8.count
        }
    }

    private func delete() async {
        do {
            try await link.send("companion.delete", ["id": summary.id])
            onChanged()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}

/// One character field: a counter, and a ✨ button that asks Gemini to write it again.
struct CharacterField: View {
    let title: String
    @Binding var text: String
    let limit: Int
    let lines: Int
    var hint: String? = nil
    var suggest: (() async -> Void)? = nil
    @State private var suggesting = false

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title).font(.lpLabel).foregroundStyle(Color.inkMuted)
                Spacer()
                if let suggest {
                    if suggesting {
                        ProgressView().controlSize(.mini)
                    } else {
                        Button {
                            suggesting = true
                            Task {
                                await suggest()
                                suggesting = false
                            }
                        } label: { Label("Suggest", systemImage: "sparkles").font(.lpCaption) }
                            .buttonStyle(.plain)
                            .foregroundStyle(DottyLight.firefly.color)
                    }
                }
                Text("\(text.count)/\(limit)").font(.lpCaption).foregroundStyle(Color.inkFaint)
            }
            TextField(title, text: $text, axis: .vertical)
                .lineLimit(lines...max(lines, 8))
                .font(.lpBody).foregroundStyle(Color.ink)
                .padding(Spacing.s)
                .background(RoundedRectangle(cornerRadius: 10).fill(Color.glassStrong))
                .onChange(of: text) { _, v in if v.count > limit { text = String(v.prefix(limit)) } }
            if let hint { Text(hint).font(.lpCaption).foregroundStyle(Color.inkFaint) }
        }
    }
}

/// A character field line by line: a pin at the left of each line (pinned = never changes by itself),
/// ✕ to delete one, "Add line" below. Lists hold up to 6 items of `limit` characters; prose fields
/// `limit` characters in all.
struct LinesField: View {
    let title: String
    @Binding var lines: [CompanionPage.Line]
    let list: Bool
    let limit: Int
    var suggest: (() async -> Void)? = nil
    @State private var suggesting = false

    private var used: Int { lines.map { $0.text.count }.reduce(0, +) + max(0, lines.count - 1) }
    private var canAdd: Bool { list ? lines.count < 6 : used < limit }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(title).font(.lpLabel).foregroundStyle(Color.inkMuted)
                Spacer()
                if let suggest {
                    if suggesting {
                        ProgressView().controlSize(.mini)
                    } else {
                        Button {
                            suggesting = true
                            Task {
                                await suggest()
                                suggesting = false
                            }
                        } label: { Label("Suggest", systemImage: "sparkles").font(.lpCaption) }
                            .buttonStyle(.plain)
                            .foregroundStyle(DottyLight.firefly.color)
                    }
                }
                Text(list ? "\(lines.count)/6" : "\(used)/\(limit)").font(.lpCaption)
                    .foregroundStyle(!list && used > limit ? DottyLight.ember.color : Color.inkFaint)
            }
            ForEach($lines) { $line in
                HStack(alignment: .firstTextBaseline, spacing: Spacing.s) {
                    Button { line.pinned.toggle() } label: {
                        Image(systemName: line.pinned ? "pin.fill" : "pin")
                            .font(.system(size: 14, weight: .semibold))
                            .foregroundStyle(line.pinned ? DottyLight.firefly.color : Color.inkFaint)
                            .frame(width: 24)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(line.pinned ? "Unpin" : "Pin")
                    TextField(list ? "A \(title.lowercased().dropLast())" : "A sentence", text: $line.text, axis: .vertical)
                        .lineLimit(1...6)
                        .font(.lpBody).foregroundStyle(Color.ink)
                        .onChange(of: line.text) { _, v in
                            let clean = v.replacingOccurrences(of: "\n", with: " ")
                            let cut = list && clean.count > limit ? String(clean.prefix(limit)) : clean
                            if cut != v { line.text = cut }
                        }
                    Button { lines.removeAll { $0.id == line.id } } label: {
                        Image(systemName: "xmark").font(.system(size: 12, weight: .semibold)).foregroundStyle(Color.inkFaint)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Delete line")
                }
                .padding(.horizontal, Spacing.s).padding(.vertical, 6)
                .background(RoundedRectangle(cornerRadius: 10).fill(Color.glassStrong))
            }
            if canAdd {
                Button { lines.append(CompanionPage.Line(text: "")) } label: {
                    Label("Add line", systemImage: "plus").font(.lpCaption)
                }
                .buttonStyle(.plain)
                .foregroundStyle(Color.inkMuted)
            }
        }
    }
}

/// The "+" page: describe who you'd like and Gemini designs them (personality, traits, quirks, way of
/// speaking, backstory, a voice and an avatar); everything stays editable before Create. No name:
/// they get one when they first wake up on Dotty.
struct NewCompanionPage: View {
    let onCreated: (String) -> Void
    @Environment(DottyLink.self) private var link
    @State private var description = ""
    @State private var designing = false
    @State private var designed = false
    @State private var creating = false
    @State private var personality = ""
    @State private var traits = ""
    @State private var quirks = ""
    @State private var speaking = ""
    @State private var backstory = ""
    @State private var voice = "Zephyr"
    @State private var voiceReason = ""
    @State private var avatar = "robot"
    @State private var error: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Spacing.xl) {
                PageHeader(title: "New companion",
                           subtitle: "Describe who you'd like to talk to, and Gemini designs them. They wake up on Dotty without a name.")
                    .padding(.top, Spacing.l)
                if let error { NoticeCard(kind: .error, text: error) }
                GlassCard(title: "Describe them") {
                    VStack(alignment: .leading, spacing: Spacing.s) {
                        TextField("A grumpy retired sea captain who secretly loves poetry", text: $description, axis: .vertical)
                            .lineLimit(3...6)
                            .font(.lpBody).foregroundStyle(Color.ink)
                            .padding(Spacing.s)
                            .background(RoundedRectangle(cornerRadius: 10).fill(Color.glassStrong))
                        HStack {
                            if designing { ProgressView().controlSize(.small); Text("Gemini is designing…").font(.lpCaption) }
                            Spacer()
                            Button(designed ? "Design again" : "Design with Gemini") { Task { await design() } }
                                .buttonStyle(.light())
                                .disabled(designing || description.trimmingCharacters(in: .whitespaces).isEmpty)
                        }
                    }
                    .padding(Spacing.m)
                }
                if designed {
                    GlassCard(title: "Voice and avatar") {
                        HStack(spacing: Spacing.m) {
                            CompanionView.avatarImage(avatar, companion: "", size: 64)
                                .padding(4)
                                .background(RoundedRectangle(cornerRadius: 12).fill(Color.white))
                            VStack(alignment: .leading, spacing: 2) {
                                Text(voice).font(.lpHeadline).foregroundStyle(Color.ink)
                                Text(voiceReason).font(.lpCaption).foregroundStyle(Color.inkMuted)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                        .padding(Spacing.m)
                        Text("You can change both on their page after creating them.")
                            .font(.lpCaption).foregroundStyle(Color.inkFaint)
                            .padding(.horizontal, Spacing.m).padding(.bottom, Spacing.s)
                    }
                    GlassCard(title: "Who they are") {
                        VStack(alignment: .leading, spacing: Spacing.m) {
                            CharacterField(title: "Personality", text: $personality, limit: 600, lines: 4,
                                           suggest: { await suggest("personality", "personality") })
                            CharacterField(title: "Traits (one per line)", text: $traits, limit: 6 * 41, lines: 3,
                                           suggest: { await suggest("traits", "set of traits") })
                            CharacterField(title: "Quirks (one per line)", text: $quirks, limit: 6 * 61, lines: 3,
                                           suggest: { await suggest("quirks", "set of quirks") })
                            CharacterField(title: "Way of speaking", text: $speaking, limit: 300, lines: 3,
                                           suggest: { await suggest("speaking", "way of speaking") })
                            CharacterField(title: "Backstory", text: $backstory, limit: 600, lines: 4,
                                           suggest: { await suggest("backstory", "backstory") })
                        }
                        .padding(Spacing.m)
                    }
                }
                HStack {
                    if creating { ProgressView().controlSize(.small) }
                    Spacer()
                    Button(designed ? "Create" : "Create a blank one") { Task { await create() } }
                        .buttonStyle(.light())
                        .disabled(creating || designing)
                }
                .needsDotty(link)
            }
            .padding(.horizontal, Spacing.l)
            .padding(.bottom, Spacing.xxl * 2)
        }
        .keyboardDismissing()
    }

    private func design() async {
        designing = true
        defer { designing = false }
        let (prompt, schema) = CompanionDesign.everything(description)
        do {
            let a = try await GeminiText.generate(prompt, schema: schema)
            personality = a["personality"] as? String ?? ""
            traits = (a["traits"] as? [String] ?? []).joined(separator: "\n")
            quirks = (a["quirks"] as? [String] ?? []).joined(separator: "\n")
            speaking = a["speaking"] as? String ?? ""
            backstory = a["backstory"] as? String ?? ""
            voice = a["voice"] as? String ?? "Zephyr"
            voiceReason = a["voiceReason"] as? String ?? ""
            avatar = a["avatar"] as? String ?? "robot"
            designed = true
            error = nil
        } catch {
            self.error = error.localizedDescription
        }
    }

    private func suggest(_ field: String, _ label: String) async {
        let current = ["personality": personality, "traits": traits, "quirks": quirks, "speaking": speaking, "backstory": backstory]
        let (prompt, schema) = CompanionDesign.field(field, label: label, description: description, current: current,
                                                     voice: voice, avatar: avatar)
        do {
            let a = try await GeminiText.generate(prompt, schema: schema)
            let value = (a["value"] as? [String])?.joined(separator: "\n") ?? a["value"] as? String ?? ""
            switch field {
            case "personality": personality = value
            case "traits": traits = value
            case "quirks": quirks = value
            case "speaking": speaking = value
            default: backstory = value
            }
            error = nil
        } catch {
            self.error = error.localizedDescription
        }
    }

    private func create() async {
        creating = true
        defer { creating = false }
        do {
            var args: [String: Any] = ["type": "custom"]
            if designed {
                args["voice"] = voice
                args["avatar"] = avatar
            }
            let r = try await link.send("companion.create", args)
            let id = r["id"] as? String ?? ""
            if designed {
                for (field, value) in [("personality", personality), ("traits", traits), ("quirks", quirks),
                                       ("speaking", speaking), ("backstory", backstory)] where !value.isEmpty {
                    try await CompanionPage.sendField(link, id: id, field: field, value: value)
                }
            }
            description = ""
            designed = false
            onCreated(id)
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}

/// The photo library for avatars, kept on this iPhone (Application Support/AvatarPhotos/<photo>/:
/// preview.png + the two bitmaps Dotty takes), usable by any companion; which photo each companion
/// wears is remembered too (Dotty only has its own copy, photo.bin). Deleting a photo here doesn't
/// change what a companion already shows on Dotty.
enum AvatarPhotos {
    static var root: URL {
        URL.applicationSupportDirectory.appending(path: "AvatarPhotos", directoryHint: .isDirectory)
    }

    /// Newest first.
    static func all() -> [String] {
        let items = (try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: [.creationDateKey])) ?? []
        return items.filter(\.hasDirectoryPath)
            .sorted { a, b in
                let da = (try? a.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast
                let db = (try? b.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast
                return da > db
            }
            .map(\.lastPathComponent)
    }

    @discardableResult
    static func add(preview: UIImage, big: Data, small: Data) -> String {
        let id = UUID().uuidString
        let folder = root.appending(path: id, directoryHint: .isDirectory)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try? preview.pngData()?.write(to: folder.appending(path: "preview.png"))
        try? big.write(to: folder.appending(path: "128.bin"))
        try? small.write(to: folder.appending(path: "48.bin"))
        return id
    }

    static func preview(_ photo: String) -> UIImage? {
        UIImage(contentsOfFile: root.appending(path: "\(photo)/preview.png").path)
    }

    static func bitmaps(_ photo: String) -> (big: Data, small: Data)? {
        guard let big = try? Data(contentsOf: root.appending(path: "\(photo)/128.bin")),
              let small = try? Data(contentsOf: root.appending(path: "\(photo)/48.bin")) else { return nil }
        return (big, small)
    }

    static func delete(_ photo: String) {
        try? FileManager.default.removeItem(at: root.appending(path: photo, directoryHint: .isDirectory))
    }

    /// The photo a companion wears (if it came from this iPhone).
    static func photo(for companion: String) -> String? {
        UserDefaults.standard.string(forKey: "avatarPhoto.\(companion)")
    }
    static func setPhoto(_ photo: String?, for companion: String) {
        UserDefaults.standard.set(photo, forKey: "avatarPhoto.\(companion)")
    }

    /// What the app shows for a companion wearing a photo.
    static func load(_ companion: String) -> UIImage? {
        photo(for: companion).flatMap(preview)
    }
    static func remove(_ companion: String) {
        setPhoto(nil, for: companion)
    }

    /// Both circles to Dotty in pieces (companion.photo; base64 + JSON stay under 512 bytes).
    static func send(_ link: DottyLink, companion: String, big: Data, small: Data,
                     progress: (Double) -> Void) async throws {
        let piece = 240  // 320 base64 characters + ~80 of JSON: well under 512 even with escapes
        let total = Double(big.count + small.count)
        var sent = 0.0
        for (size, data) in [(128, big), (48, small)] {
            var offset = 0
            while offset < data.count {
                let end = min(offset + piece, data.count)
                try await link.send("companion.photo", ["id": companion, "size": size, "offset": offset,
                                                        "data": data.subdata(in: offset..<end).base64EncodedString()])
                sent += Double(end - offset)
                progress(sent / total)
                offset = end
            }
        }
    }
}

/// The pages as avatar circles (the user, 2026-10-24): 28 px, a ring in one of the design system's colours
/// by companion order, a full ring on the page shown, the others dimmed; a dot under the one running on
/// Dotty; a dashed "+" for a new one. A tap jumps to that page (swiping still works).
struct CompanionPager: View {
    let companions: [CompanionView.Summary]
    @Binding var selection: String
    let active: String
    let canAdd: Bool
    let newPage: String

    var body: some View {
        HStack(spacing: 10) {
            ForEach(Array(companions.enumerated()), id: \.element.id) { index, c in
                let colour = DottyLight.allCases[index % DottyLight.allCases.count].color
                let shown = selection == c.id
                Button { withAnimation { selection = c.id } } label: {
                    VStack(spacing: 3) {
                        CompanionView.avatarImage(c.avatar, companion: c.id, size: 24)
                            .clipShape(Circle())
                            .frame(width: 28, height: 28)
                            .overlay(Circle().strokeBorder(colour.opacity(shown ? 1 : 0.5), lineWidth: shown ? 2.5 : 1))
                            .opacity(shown ? 1 : 0.55)
                        Circle().fill(c.id == active ? colour : .clear).frame(width: 4, height: 4)
                    }
                }
                .buttonStyle(.plain)
                .accessibilityLabel(c.name.isEmpty ? "Companion \(index + 1)" : c.name)
            }
            if canAdd {
                Button { withAnimation { selection = newPage } } label: {
                    VStack(spacing: 3) {
                        Image(systemName: "plus").font(.system(size: 12, weight: .bold)).foregroundStyle(Color.inkMuted)
                            .frame(width: 28, height: 28)
                            .overlay(Circle().strokeBorder(Color.inkMuted, style: StrokeStyle(lineWidth: selection == newPage ? 2 : 1, dash: [3, 3])))
                        Circle().fill(.clear).frame(width: 4, height: 4)
                    }
                }
                .buttonStyle(.plain)
                .accessibilityLabel("New companion")
            }
        }
    }
}
