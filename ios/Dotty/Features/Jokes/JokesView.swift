import SwiftUI

/// The Joke Factory cartridge's screen: how many jokes Dotty keeps offline (and how many are
/// still unread per category), a manual refresh, and the favourites starred on Dotty
/// (firmware: cartridges/jokes/).
struct JokesView: View {
    @Environment(DottyLink.self) private var link
    @State private var status: Status?
    @State private var favourites: [Favourite] = []
    @State private var expanded: Set<Int> = []
    @State private var error: String?

    struct Status {
        var count = 0
        var unseen: [(String, Int)] = []
        var favourites = 0
        var syncing = false
        var done = 0
        var total = 0
        var syncedAt: Date?
        var syncError: String?
    }

    struct Favourite: Identifiable {
        let id: Int
        let category: String
        let setup: String
        let punchline: String
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Joke Factory", subtitle: "Every joke lives on Dotty's SD card, so it works offline.")
                        .padding(.top, Spacing.l)
                    NotConnectedNotice()
                    if let error { NoticeCard(kind: .error, text: error) }
                    if let status { statusCard(status) }
                    favouritesCard
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
            if link.lastEvent?.event == "jokes.changed" { Task { await load() } }
        }
        // While Dotty downloads, follow its progress.
        .task(id: status?.syncing == true) {
            while status?.syncing == true, !Task.isCancelled {
                try? await Task.sleep(for: .seconds(2))
                await loadStatus()
            }
        }
    }

    // MARK: - Cards

    private func statusCard(_ status: Status) -> some View {
        GlassCard(title: status.count == 0 ? "No jokes yet" : "\(status.count) jokes on Dotty") {
            if status.count > 0 {
                LightRow(title: "Still unread", systemImage: "sparkles") {
                    Text(status.unseen.map { "\($0.0) \($0.1)" }.joined(separator: " · "))
                        .font(.lpCaption)
                        .lineLimit(2)
                        .multilineTextAlignment(.trailing)
                }
            }
            if status.syncing {
                VStack(alignment: .leading, spacing: Spacing.s) {
                    LightProgress(value: status.total > 0 ? Double(status.done) / Double(status.total) : 0)
                    Text("Dotty is getting the jokes over Wi-Fi").font(.lpCaption).foregroundStyle(Color.inkMuted)
                }
                .padding(Spacing.m)
            } else {
                LightRow(title: "Get new jokes",
                         subtitle: updatedLine(status),
                         systemImage: "arrow.down.circle",
                         action: { Task { await sync() } })
            }
            if let syncError = status.syncError, !status.syncing {
                Text("Last try: \(syncError)").font(.lpCaption).foregroundStyle(DottyLight.ember.color)
                    .padding(.horizontal, Spacing.m).padding(.bottom, Spacing.s)
            }
        }
    }

    private func updatedLine(_ status: Status) -> String {
        guard let date = status.syncedAt else { return "Dotty downloads them once, then checks weekly" }
        return "Updated \(date.formatted(.relative(presentation: .named))); checks weekly"
    }

    private var favouritesCard: some View {
        GlassCard(title: "Favourites") {
            if favourites.isEmpty {
                LightRow(title: "No favourites yet", subtitle: "Tap the star on a joke on Dotty", systemImage: "star")
            }
            ForEach(favourites) { joke in
                let open = expanded.contains(joke.id)
                Button {
                    withAnimation(.settle) {
                        if open { expanded.remove(joke.id) } else { expanded.insert(joke.id) }
                    }
                } label: {
                    VStack(alignment: .leading, spacing: Spacing.xs) {
                        Text(joke.category.uppercased())
                            .font(.lpLabel)
                            .foregroundStyle(DottyLight.firefly.color)
                        Text(joke.setup)
                            .font(.lpBody)
                            .foregroundStyle(Color.ink)
                            .multilineTextAlignment(.leading)
                            .fixedSize(horizontal: false, vertical: true)
                        if !joke.punchline.isEmpty {
                            if open {
                                Text(joke.punchline)
                                    .font(.lpHeadline)
                                    .foregroundStyle(Color.ink)
                                    .fixedSize(horizontal: false, vertical: true)
                                    .transition(.opacity)
                            } else {
                                Text("Tap for the punchline").font(.lpCaption).foregroundStyle(Color.inkMuted)
                            }
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(Spacing.m)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .contextMenu {
                    Button("Remove from favourites", systemImage: "star.slash", role: .destructive) {
                        Task { await remove(joke) }
                    }
                }
            }
            if !favourites.isEmpty {
                Text("Long-press a joke to remove it.")
                    .font(.lpCaption).foregroundStyle(Color.inkFaint)
                    .padding(.horizontal, Spacing.m).padding(.bottom, Spacing.s)
            }
        }
    }

    // MARK: - Dotty

    private func load() async {
        await loadStatus()
        do {
            let reply = try await link.send("jokes.favourites")
            favourites = (reply["favourites"] as? [[String: Any]] ?? []).compactMap { item in
                guard let id = item["id"] as? Int else { return nil }
                return Favourite(id: id, category: item["category"] as? String ?? "",
                                 setup: item["setup"] as? String ?? "", punchline: item["punchline"] as? String ?? "")
            }
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func loadStatus() async {
        do {
            let reply = try await link.send("jokes.status")
            var s = Status()
            s.count = reply["count"] as? Int ?? 0
            let unseen = reply["unseen"] as? [String: Int] ?? [:]
            s.unseen = ["Dark", "Programming", "Misc", "Any"].compactMap { name in unseen[name].map { (name, $0) } }
            s.favourites = reply["favourites"] as? Int ?? 0
            s.syncing = reply["syncing"] as? Bool ?? false
            s.done = reply["done"] as? Int ?? 0
            s.total = reply["total"] as? Int ?? 0
            if let at = reply["syncedAt"] as? Double, at > 1_600_000_000 { s.syncedAt = Date(timeIntervalSince1970: at) }
            s.syncError = reply["syncError"] as? String
            status = s
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func sync() async {
        do {
            try await link.send("jokes.sync")
            await loadStatus()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func remove(_ joke: Favourite) async {
        do {
            try await link.send("jokes.favourite.remove", ["id": joke.id])
            withAnimation(.settle) { favourites.removeAll { $0.id == joke.id } }
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}
