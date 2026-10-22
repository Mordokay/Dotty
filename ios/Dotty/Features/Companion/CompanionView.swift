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
                .tabViewStyle(.page(indexDisplayMode: .always))
                .indexViewStyle(.page(backgroundDisplayMode: .always))
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

    struct Profile: Equatable {
        var name = "", personality = "", speaking = "", backstory = "", traits = "", quirks = ""
        var voice = "Zephyr", avatar = "robot"
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
                avatarCard.needsDotty(link)
                voiceCard.needsDotty(link)
                characterCard.needsDotty(link)
                GlassCard(title: "What they know") {
                    NavigationLink { MemoriesView(companion: summary.id) } label: {
                        LightRow(title: summary.memories == 1 ? "1 memory" : "\(summary.memories) memories",
                                 subtitle: "Things \(displayName) remembers about you", systemImage: "bookmark") { chevron }
                    }
                    .buttonStyle(.plain)
                    NavigationLink { CompanionHistoryView(companion: summary.id) } label: {
                        LightRow(title: "Past chats", subtitle: "What you said and what \(displayName) answered",
                                 systemImage: "clock.arrow.circlepath") { chevron }
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
            }
            .padding(Spacing.m)
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
                field("Name", text: $profile.name, limit: 24, lines: 1,
                      hint: summary.awake ? nil : "Usually given when they first wake up on Dotty")
                field("Personality", text: $profile.personality, limit: 600, lines: 4)
                field("Traits (one per line)", text: $profile.traits, limit: 6 * 41, lines: 3)
                field("Quirks (one per line)", text: $profile.quirks, limit: 6 * 61, lines: 3)
                field("Way of speaking", text: $profile.speaking, limit: 300, lines: 3)
                field("Backstory", text: $profile.backstory, limit: 600, lines: 4)
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

    private func field(_ title: String, text: Binding<String>, limit: Int, lines: Int, hint: String? = nil) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title).font(.lpLabel).foregroundStyle(Color.inkMuted)
                Spacer()
                Text("\(text.wrappedValue.count)/\(limit)").font(.lpCaption).foregroundStyle(Color.inkFaint)
            }
            TextField(title, text: text, axis: .vertical)
                .lineLimit(lines...max(lines, 8))
                .font(.lpBody).foregroundStyle(Color.ink)
                .padding(Spacing.s)
                .background(RoundedRectangle(cornerRadius: 10).fill(Color.glassStrong))
                .onChange(of: text.wrappedValue) { _, v in if v.count > limit { text.wrappedValue = String(v.prefix(limit)) } }
            if let hint { Text(hint).font(.lpCaption).foregroundStyle(Color.inkFaint) }
        }
    }

    // MARK: - Dotty

    private func load() async {
        do {
            let r = try await link.send("companion.get", ["id": summary.id])
            let c = r["companion"] as? [String: Any] ?? [:]
            var p = Profile()
            p.name = c["name"] as? String ?? ""
            p.personality = c["personality"] as? String ?? ""
            p.speaking = c["speaking"] as? String ?? ""
            p.backstory = c["backstory"] as? String ?? ""
            p.traits = (c["traits"] as? [String] ?? []).joined(separator: "\n")
            p.quirks = (c["quirks"] as? [String] ?? []).joined(separator: "\n")
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

    /// Changed fields only; a long one goes in pieces (a command carries at most 512 bytes).
    private func save() async {
        saving = true
        defer { saving = false }
        let pairs: [(String, String, String)] = [
            ("name", profile.name, saved.name), ("personality", profile.personality, saved.personality),
            ("traits", profile.traits, saved.traits), ("quirks", profile.quirks, saved.quirks),
            ("speaking", profile.speaking, saved.speaking), ("backstory", profile.backstory, saved.backstory),
        ]
        do {
            for (field, value, old) in pairs where value != old {
                try await CompanionPage.sendField(link, id: summary.id, field: field, value: value)
            }
            await load()
            onChanged()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    static func sendField(_ link: DottyLink, id: String, field: String, value: String) async throws {
        if value.utf8.count <= 300 {
            try await link.send("companion.update", ["id": id, field: value])
            return
        }
        var pieces: [String] = []
        var current = ""
        for ch in value {  // whole characters, ≤ 300 bytes a piece
            if (current + String(ch)).utf8.count > 300 {
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
            AvatarPhotos.remove(summary.id)
            onChanged()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}

/// The "+" page: a new custom companion (Task 9 adds the Gemini co-designer here).
struct NewCompanionPage: View {
    let onCreated: (String) -> Void
    @Environment(DottyLink.self) private var link
    @State private var error: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Spacing.xl) {
                PageHeader(title: "New companion", subtitle: "Someone new to talk to. They wake up on Dotty without a name.")
                    .padding(.top, Spacing.l)
                if let error { NoticeCard(kind: .error, text: error) }
                Button("Create") { Task { await create() } }.buttonStyle(.light()).needsDotty(link)
            }
            .padding(.horizontal, Spacing.l)
        }
    }

    private func create() async {
        do {
            let r = try await link.send("companion.create", ["type": "custom"])
            onCreated(r["id"] as? String ?? "")
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}

/// Photos picked as avatars, kept on this iPhone (Application Support/AvatarPhotos/<id>.png) so the
/// app can show them; Dotty has its own dithered copy.
enum AvatarPhotos {
    static var folder: URL {
        URL.applicationSupportDirectory.appending(path: "AvatarPhotos", directoryHint: .isDirectory)
    }
    static func load(_ id: String) -> UIImage? {
        UIImage(contentsOfFile: folder.appending(path: "\(id).png").path)
    }
    static func save(_ image: UIImage, for id: String) {
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try? image.pngData()?.write(to: folder.appending(path: "\(id).png"))
    }
    static func remove(_ id: String) {
        try? FileManager.default.removeItem(at: folder.appending(path: "\(id).png"))
    }
}
