import SwiftUI

/// The AI companion's screen (firmware: cartridges/companion/): how today went, the avatar, the
/// voice, out loud or text only, the volume, the Gemini key and past chats. Dotty switches
/// Bluetooth off while it talks to Gemini (they can't share its memory), so settings reach it
/// between chats.
struct CompanionView: View {
    @Environment(DottyLink.self) private var link
    @State private var status: Status?
    @State private var error: String?
    @State private var keyDraft = ""
    @State private var volume: Double = 70
    @State private var volumeTask: Task<Void, Never>?
    @State private var confirmForget = false

    struct Status {
        var state = "offline", avatar = "robot", voice = "Zephyr"
        var favourites: [String] = []
        var volume = 70
        var textOnly = false, keySet = false
        var keyHint: String?
        var turnsToday = 0, tokensToday = 0
        var outUntil: Date?
    }

    /// The avatars in the firmware's order (tools/avatar_art.py), pictures in Resources/Avatars.
    static let avatars: [(id: String, name: String)] = [
        ("robot", "Robot"), ("dog", "Dog"), ("cat", "Cat"), ("fox", "Fox"), ("owl", "Owl"),
        ("bear", "Bear"), ("girl", "Girl"), ("guy", "Guy"), ("ghost", "Ghost"), ("dragon", "Dragon"),
    ]

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Companion", subtitle: "Hold BOOT on Dotty and talk. It answers out loud in about a second.")
                        .padding(.top, Spacing.l)
                    NotConnectedNotice()
                    if let error { NoticeCard(kind: .error, text: error) }
                    if let status {
                        todayCard(status)
                        avatarCard(status).needsDotty(link)
                        voiceCard(status).needsDotty(link)
                        keyCard(status).needsDotty(link)
                        historyCard.needsDotty(link)
                    }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await load() }
        }
        .navigationTitle("")
        .task { await load() }
        .onChange(of: link.connection) { _, state in
            if state == .connected { Task { await load() } }
        }
        .onChange(of: link.eventCount) { _, _ in
            if link.lastEvent?.event == "companion.changed" { Task { await load() } }
        }
        .confirmationDialog("Forget every chat?", isPresented: $confirmForget, titleVisibility: .visible) {
            Button("Forget all", role: .destructive) { Task { await send("companion.forget") } }
        } message: {
            Text("Dotty deletes its chat history from the SD card.")
        }
    }

    // MARK: - Cards

    private func todayCard(_ s: Status) -> some View {
        GlassCard(title: "Today") {
            if let until = s.outUntil, until > .now {
                LightRow(title: "Out of words until \(until.formatted(date: .omitted, time: .shortened))",
                         subtitle: "Google's free allowance is used up for now", systemImage: "moon.zzz")
            }
            LightRow(title: s.turnsToday == 1 ? "1 chat" : "\(s.turnsToday) chats",
                     subtitle: "\(s.tokensToday.formatted()) tokens · the free allowance resets at midnight in California",
                     systemImage: "bubble.left.and.bubble.right")
            if !s.keySet {
                LightRow(title: "Add your Gemini key below", subtitle: "Dotty needs it to talk", systemImage: "key")
            }
        }
    }

    private func avatarCard(_ s: Status) -> some View {
        GlassCard(title: "Avatar") {
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: Spacing.s), count: 5), spacing: Spacing.s) {
                ForEach(Self.avatars, id: \.id) { a in
                    Button {
                        Task { await send("companion.settings", ["avatar": a.id]) }
                    } label: {
                        VStack(spacing: 2) {
                            avatarImage(a.id)
                                .padding(4)
                                .background(RoundedRectangle(cornerRadius: 12).fill(Color.white))
                                .overlay(RoundedRectangle(cornerRadius: 12)
                                    .strokeBorder(a.id == s.avatar ? DottyLight.firefly.color : Color.glassEdge,
                                                  lineWidth: a.id == s.avatar ? 3 : 1))
                            Text(a.name).font(.lpCaption).foregroundStyle(a.id == s.avatar ? Color.ink : Color.inkMuted)
                        }
                    }
                    .buttonStyle(.plain)
                    .accessibilityAddTraits(a.id == s.avatar ? .isSelected : [])
                }
            }
            .padding(Spacing.m)
        }
    }

    private func avatarImage(_ id: String) -> some View {
        Group {
            if let image = UIImage(named: id) {
                Image(uiImage: image).interpolation(.none).resizable().scaledToFit()
            } else {
                Image(systemName: "person.crop.circle").resizable().scaledToFit().foregroundStyle(Color.inkMuted)
            }
        }
        .frame(width: 52, height: 52)
    }

    private func voiceCard(_ s: Status) -> some View {
        GlassCard(title: "Voice") {
            NavigationLink {
                VoicesView(selected: s.voice, favourites: s.favourites,
                           onPick: { name in Task { await send("companion.settings", ["voice": name]) } },
                           onFavourites: { list in Task { await send("companion.settings", ["favourites": list]) } })
            } label: {
                LightRow(title: s.voice, subtitle: "Star voices to step through them on Dotty's settings screen",
                         systemImage: "waveform") {
                    Image(systemName: "chevron.right").font(.system(size: 14, weight: .semibold, design: .rounded))
                }
            }
            .buttonStyle(.plain)
            Toggle(isOn: Binding(get: { !s.textOnly },
                                 set: { on in Task { await send("companion.settings", ["textOnly": !on]) } })) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Speak out loud").font(.lpHeadline).foregroundStyle(Color.ink)
                    Text("Off: replies are text on Dotty's screen only.").font(.lpCaption).foregroundStyle(Color.inkMuted)
                }
            }
            .toggleStyle(.light())
            .padding(Spacing.m)
            LightSlider(title: "Volume", value: $volume)
                .padding(.horizontal, Spacing.m)
                .padding(.bottom, Spacing.m)
                .onChange(of: volume) { _, value in
                    guard Int(value) != status?.volume else { return }
                    volumeTask?.cancel()
                    volumeTask = Task {
                        try? await Task.sleep(for: .milliseconds(400))  // one command when the finger rests
                        guard !Task.isCancelled else { return }
                        await send("companion.settings", ["volume": Int(value)])
                        volumeTask = nil
                    }
                }
        }
    }

    private func keyCard(_ s: Status) -> some View {
        GlassCard(title: "Gemini key") {
            if s.keySet, let hint = s.keyHint {
                LightRow(title: "•••• \(hint)", subtitle: "Saved on Dotty. Paste a new one to replace it.", systemImage: "key.fill")
            }
            HStack(spacing: Spacing.s) {
                SecureField(s.keySet ? "New key" : "Paste your key", text: $keyDraft)
                    .font(.lpBody).foregroundStyle(Color.ink)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("Save") {
                    let key = keyDraft
                    keyDraft = ""
                    Task { await send("companion.settings", ["key": key]) }
                }
                .buttonStyle(.quiet())
                .disabled(keyDraft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }
            .padding(Spacing.m)
            Link(destination: URL(string: "https://aistudio.google.com/apikey")!) {
                LightRow(title: "Get a free key", subtitle: "Google AI Studio › Get API key", systemImage: "arrow.up.right.square")
            }
        }
    }

    private var historyCard: some View {
        GlassCard(title: "Chats") {
            NavigationLink {
                CompanionHistoryView()
            } label: {
                LightRow(title: "Past chats", subtitle: "What you said and what Dotty answered, by day",
                         systemImage: "clock.arrow.circlepath") {
                    Image(systemName: "chevron.right").font(.system(size: 14, weight: .semibold, design: .rounded))
                }
            }
            .buttonStyle(.plain)
            LightRow(title: "Forget every chat", systemImage: "trash", action: { confirmForget = true })
        }
    }

    // MARK: - Dotty

    private func load() async {
        do {
            let r = try await link.send("companion.status")
            var s = Status()
            s.state = r["state"] as? String ?? "offline"
            s.avatar = r["avatar"] as? String ?? "robot"
            s.voice = r["voice"] as? String ?? "Zephyr"
            s.favourites = r["favourites"] as? [String] ?? []
            s.volume = r["volume"] as? Int ?? 70
            s.textOnly = r["textOnly"] as? Bool ?? false
            s.keySet = r["keySet"] as? Bool ?? false
            s.keyHint = r["keyHint"] as? String
            let today = r["today"] as? [String: Any] ?? [:]
            s.turnsToday = today["turns"] as? Int ?? 0
            s.tokensToday = today["tokens"] as? Int ?? 0
            if let out = r["outUntil"] as? Int { s.outUntil = Date(timeIntervalSince1970: TimeInterval(out)) }
            status = s
            if volumeTask == nil { volume = Double(s.volume) }
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func send(_ cmd: String, _ args: [String: Any] = [:]) async {
        do {
            let r = try await link.send(cmd, args)
            if r["ok"] as? Bool == false {
                error = (r["error"] as? String).map { "Dotty said: \($0)" } ?? "Dotty couldn't do that"
            }
            await load()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}
