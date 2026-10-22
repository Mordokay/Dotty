import SwiftUI

/// What all the companions share (behind the gear on the Companions screen): the Gemini keys —
/// up to 3, one per session in turn on Dotty, each checked with Google before it's kept — out
/// loud or text only, the volume, and today's use.
struct CompanionSettingsSheet: View {
    @Environment(DottyLink.self) private var link
    @Environment(\.dismiss) private var dismiss
    @State private var keys: [DottyKey] = []
    @State private var textOnly = false
    @State private var volume: Double = 70
    @State private var volumeTask: Task<Void, Never>?
    @State private var turnsToday = 0
    @State private var tokensToday = 0
    @State private var draft = ""
    @State private var checking = false
    @State private var message: String?
    @State private var error: String?

    struct DottyKey: Identifiable {
        var id: String { hint }
        let hint: String
        let turns: Int, tokens: Int
        let outUntil: Date?
    }

    var body: some View {
        NavigationStack {
            LightField {
                ScrollView {
                    VStack(alignment: .leading, spacing: Spacing.xl) {
                        NotConnectedNotice()
                        if let error { NoticeCard(kind: .error, text: error) }
                        keysCard.needsDotty(link)
                        voiceCard.needsDotty(link)
                        GlassCard(title: "Today") {
                            LightRow(title: turnsToday == 1 ? "1 chat" : "\(turnsToday) chats",
                                     subtitle: "\(tokensToday.formatted()) tokens · free allowances reset at midnight in California",
                                     systemImage: "bubble.left.and.bubble.right")
                        }
                    }
                    .padding(.horizontal, Spacing.l)
                    .padding(.vertical, Spacing.l)
                }
            }
            .navigationTitle("Companion settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }
        .task { await load() }
    }

    private var keysCard: some View {
        GlassCard(title: "Gemini keys") {
            ForEach(keys) { key in
                LightRow(title: "•••• \(key.hint)", subtitle: subtitle(key), systemImage: "key.fill") {
                    Button("Remove", role: .destructive) { Task { await remove(key) } }
                        .buttonStyle(.quiet())
                }
            }
            if keys.count < 3 {
                HStack(spacing: Spacing.s) {
                    SecureField(keys.isEmpty ? "Paste your key" : "Add another key", text: $draft)
                        .font(.lpBody).foregroundStyle(Color.ink)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .disabled(checking)
                    if checking {
                        ProgressView().controlSize(.small)
                    } else {
                        Button("Add") { Task { await add() } }
                            .buttonStyle(.quiet())
                            .disabled(draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    }
                }
                .padding(Spacing.m)
                if checking {
                    Text("Checking the key with Google…").font(.lpCaption).foregroundStyle(Color.inkMuted)
                        .padding(.horizontal, Spacing.m).padding(.bottom, Spacing.s)
                }
            }
            if let message {
                Text(message).font(.lpCaption).foregroundStyle(DottyLight.ember.color)
                    .padding(.horizontal, Spacing.m).padding(.bottom, Spacing.s)
            }
            Text("Dotty uses one key per chat, taking turns. A key out of its free allowance waits until it resets.")
                .font(.lpCaption).foregroundStyle(Color.inkFaint)
                .padding(.horizontal, Spacing.m).padding(.bottom, Spacing.s)
            Link(destination: URL(string: "https://aistudio.google.com/apikey")!) {
                LightRow(title: "Get a free key", subtitle: "Google AI Studio › Get API key", systemImage: "arrow.up.right.square")
            }
        }
    }

    private func subtitle(_ key: DottyKey) -> String {
        if let out = key.outUntil, out > .now { return "Out of words until \(out.formatted(date: .omitted, time: .shortened))" }
        let known = GeminiKeys.load().contains { GeminiKeys.hint($0) == key.hint }
        return "\(key.turns) chats · \(key.tokens.formatted()) tokens today" + (known ? "" : " · not on this iPhone")
    }

    private var voiceCard: some View {
        GlassCard(title: "Voice") {
            Toggle(isOn: Binding(get: { !textOnly }, set: { on in
                textOnly = !on
                Task { await send("companion.settings", ["textOnly": !on]) }
            })) {
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
                    volumeTask?.cancel()
                    volumeTask = Task {
                        try? await Task.sleep(for: .milliseconds(400))
                        guard !Task.isCancelled else { return }
                        await send("companion.settings", ["volume": Int(value)])
                        volumeTask = nil
                    }
                }
        }
    }

    // MARK: - Keys

    /// Checked with Google first (a spinner meanwhile); only a key Google accepts is kept: on this
    /// iPhone's Keychain and on Dotty.
    private func add() async {
        let key = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        message = nil
        checking = true
        let check = await GeminiKeys.validate(key)
        checking = false
        switch check {
        case .offline: message = "Couldn't reach Google to check the key. Try again when you're online."
        case .refused(let why): message = "Google didn't accept this key: \(why)"
        case .ok:
            var all = GeminiKeys.load().filter { $0 != key }
            all.append(key)
            all = Array(all.suffix(3))
            if await send("companion.settings", ["keys": all]) {
                GeminiKeys.save(all)
                draft = ""
            }
        }
    }

    private func remove(_ key: DottyKey) async {
        let rest = GeminiKeys.load().filter { GeminiKeys.hint($0) != key.hint }
        if await send("companion.settings", ["keys": rest]) { GeminiKeys.save(rest) }
    }

    // MARK: - Dotty

    private func load() async {
        do {
            let r = try await link.send("companion.status")
            keys = (r["keys"] as? [[String: Any]] ?? []).map { k in
                DottyKey(hint: k["hint"] as? String ?? "", turns: k["turns"] as? Int ?? 0, tokens: k["tokens"] as? Int ?? 0,
                         outUntil: (k["outUntil"] as? Int).map { Date(timeIntervalSince1970: TimeInterval($0)) })
            }
            textOnly = r["textOnly"] as? Bool ?? false
            if volumeTask == nil { volume = Double(r["volume"] as? Int ?? 70) }
            let today = r["today"] as? [String: Any] ?? [:]
            turnsToday = today["turns"] as? Int ?? 0
            tokensToday = today["tokens"] as? Int ?? 0
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    @discardableResult
    private func send(_ cmd: String, _ args: [String: Any]) async -> Bool {
        do {
            try await link.send(cmd, args)
            await load()
            return true
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
            return false
        }
    }
}
