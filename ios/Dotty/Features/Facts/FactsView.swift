import SwiftUI

/// The Facts cartridge's screen: ten thousand facts built into Dotty (firmware:
/// cartridges/facts/), how many of each category are still new, a new fact on Dotty now, and
/// the favourites starred there.
struct FactsView: View {
    @Environment(DottyLink.self) private var link
    @State private var status: Status?
    @State private var favourites: [Fact] = []
    @State private var error: String?

    struct Status {
        var count = 0, seen = 0
        var categories: [Category] = []
        var showing: Fact?
    }

    struct Category: Identifiable {
        var id: String { key }
        let key: String, name: String
        let count: Int, seen: Int
    }

    struct Fact: Identifiable {
        let id: Int
        let text: String
        let category: String
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Facts", subtitle: "Ten thousand things to say at dinner, built into Dotty. They never need updating.")
                        .padding(.top, Spacing.l)
                    NotConnectedNotice()
                    if let error { NoticeCard(kind: .error, text: error) }
                    if let status {
                        nowCard(status).needsDotty(link)
                        categoriesCard(status).needsDotty(link)
                    }
                    favouritesCard.needsDotty(link)
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
            if link.lastEvent?.event == "facts.changed" { Task { await load() } }
        }
    }

    // MARK: - Cards

    private func nowCard(_ s: Status) -> some View {
        GlassCard(title: "On Dotty") {
            if let fact = s.showing {
                factText(fact)
            }
            LightRow(title: "Show a new fact", subtitle: "Any category, one you haven't seen yet",
                     systemImage: "sparkles", action: { Task { await next(nil) } })
            LightRow(title: "\(s.seen) of \(s.count) seen", systemImage: "eye")
        }
    }

    private func categoriesCard(_ s: Status) -> some View {
        GlassCard(title: "Categories") {
            ForEach(s.categories) { c in
                LightRow(title: c.name, subtitle: "\(c.count - c.seen) new of \(c.count)",
                         systemImage: symbol(c.key), action: { Task { await next(c.key) } })
            }
            Text("Tap one to show a fact from it on Dotty.")
                .font(.lpCaption).foregroundStyle(Color.inkFaint)
                .padding(.horizontal, Spacing.m).padding(.bottom, Spacing.s)
        }
    }

    private var favouritesCard: some View {
        GlassCard(title: "Favourites") {
            if favourites.isEmpty {
                LightRow(title: "No favourites yet", subtitle: "Tap the star on a fact on Dotty", systemImage: "star")
            }
            ForEach(favourites) { fact in
                factText(fact)
                    .contextMenu {
                        ShareLink(item: fact.text) { Label("Share", systemImage: "square.and.arrow.up") }
                        Button("Remove from favourites", systemImage: "star.slash", role: .destructive) {
                            Task { await unstar(fact) }
                        }
                    }
            }
            if !favourites.isEmpty {
                Text("Long-press a fact to share or remove it.")
                    .font(.lpCaption).foregroundStyle(Color.inkFaint)
                    .padding(.horizontal, Spacing.m).padding(.bottom, Spacing.s)
            }
        }
    }

    private func factText(_ fact: Fact) -> some View {
        VStack(alignment: .leading, spacing: Spacing.xs) {
            Text(fact.category.uppercased()).font(.lpLabel).foregroundStyle(DottyLight.firefly.color)
            Text(fact.text)
                .font(.lpBody).foregroundStyle(Color.ink)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(Spacing.m)
    }

    private func symbol(_ key: String) -> String {
        switch key {
        case "space": "moon.stars"
        case "animals": "pawprint"
        case "body": "heart"
        case "history": "building.columns"
        case "sea": "fish"
        case "mind": "brain.head.profile"
        case "earth": "mountain.2"
        case "tech": "cpu"
        case "ancient": "scroll"
        case "bugs": "ladybug"
        case "physics": "atom"
        case "art": "paintpalette"
        case "prehistoric": "fossil.shell"
        case "plants": "leaf"
        case "words": "character.book.closed"
        case "world": "globe.europe.africa"
        case "chemistry": "flask"
        case "inventions": "lightbulb"
        case "microbes": "allergens"
        case "math": "function"
        default: "sparkles"
        }
    }

    // MARK: - Dotty

    private func fact(from item: [String: Any]?) -> Fact? {
        guard let item, let id = item["id"] as? Int else { return nil }
        return Fact(id: id, text: item["text"] as? String ?? "", category: item["category"] as? String ?? "")
    }

    private func load() async {
        do {
            let r = try await link.send("facts.status")
            var s = Status()
            s.count = r["count"] as? Int ?? 0
            s.seen = r["seen"] as? Int ?? 0
            s.categories = (r["categories"] as? [[String: Any]] ?? []).map { c in
                Category(key: c["key"] as? String ?? "", name: c["name"] as? String ?? "",
                         count: c["count"] as? Int ?? 0, seen: c["seen"] as? Int ?? 0)
            }
            s.showing = fact(from: r["showing"] as? [String: Any])
            status = s
            let f = try await link.send("facts.favourites")
            favourites = (f["facts"] as? [[String: Any]] ?? []).compactMap { fact(from: $0) }
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func next(_ category: String?) async {
        do {
            try await link.send("facts.next", category.map { ["category": $0] } ?? [:])
            await load()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func unstar(_ fact: Fact) async {
        do {
            try await link.send("facts.favourite", ["id": fact.id, "on": false])
            await load()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}
