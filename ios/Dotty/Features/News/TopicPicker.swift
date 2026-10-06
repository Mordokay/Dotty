import SwiftUI

/// Picks a News topic: a Kagi News daily briefing (its categories come straight from
/// kite.kagi.com, read by this iPhone) or an outlet's feed (the list Dotty knows, grouped by
/// outlet). Several can be added in one go; added ones are marked.
struct TopicPicker: View {
    enum Kind: String, Identifiable {
        case briefing, outlet
        var id: String { rawValue }
    }

    struct KagiCategory: Identifiable, Hashable {
        var id: String { file }
        let name: String
        let file: String
    }

    let kind: Kind
    let outlets: [NewsView.Section]
    /// Keys of the topics Dotty already follows ("s:nyt-world", "c:world.json").
    let taken: Set<String>
    let busy: String?
    let full: Bool
    let onPick: (_ key: String, _ args: [String: String]) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var query = ""
    @State private var categories: [KagiCategory] = []
    @State private var failed = false

    var body: some View {
        VStack(alignment: .leading, spacing: Spacing.l) {
            VStack(alignment: .leading, spacing: Spacing.xs) {
                Text(kind == .briefing ? "Daily briefing" : "News outlet").font(.lpTitle).foregroundStyle(Color.ink)
                Text(kind == .briefing
                     ? "Kagi News picks the day's big stories in each category, summarised from dozens of outlets. Once a day."
                     : "An outlet's own feed: headline and a short summary, fresh all day.")
                    .font(.lpCaption).foregroundStyle(Color.inkMuted)
            }
            TextField(kind == .briefing ? "Search 180 categories, e.g. Formula 1" : "Search outlets and sections", text: $query)
                .font(.lpBody)
                .foregroundStyle(Color.ink)
                .autocorrectionDisabled()
                .submitLabel(.done)
                .padding(Spacing.l)
                .glassSurface(cornerRadius: Radius.soft)
            if full {
                Text("Dotty follows 12 topics at most. Remove one to add another.")
                    .font(.lpCallout).foregroundStyle(DottyLight.ember.color)
            }
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.l) {
                    if kind == .briefing { briefingList } else { outletList }
                }
            }
            .scrollDismissesKeyboard(.immediately)
            HStack {
                Spacer()
                Button("Done") { dismiss() }.buttonStyle(.light())
            }
        }
        .padding(Spacing.xl)
        .glassSurface(cornerRadius: Radius.sheet, frosted: true)
        .padding(Spacing.s)
        .task { if kind == .briefing { await loadCategories() } }
    }

    // MARK: - Lists

    @ViewBuilder private var briefingList: some View {
        if failed {
            Text("Couldn't reach Kagi News. Check this iPhone's internet connection.")
                .font(.lpCallout).foregroundStyle(DottyLight.ember.color)
        } else if categories.isEmpty {
            HStack { Spacer(); ProgressView(); Spacer() }.padding(Spacing.l)
        }
        let shown = categories.filter { matches($0.name) }
        VStack(spacing: 0) {
            ForEach(shown) { category in
                row(title: category.name, subtitle: nil, key: "c:" + category.file,
                    args: ["kagi": category.file, "name": category.name])
            }
        }
    }

    @ViewBuilder private var outletList: some View {
        // In Dotty's order, one group per outlet.
        let names = outlets.reduce(into: [String]()) { list, feed in
            if !list.contains(feed.outlet) { list.append(feed.outlet) }
        }
        ForEach(names, id: \.self) { outlet in
            let feeds = outlets.filter { $0.outlet == outlet && matches($0.name) }
            if !feeds.isEmpty {
                GlassCard(title: outlet) {
                    ForEach(feeds) { feed in
                        row(title: feed.section.isEmpty ? "Latest" : feed.section, subtitle: nil, key: "s:" + feed.id,
                            args: ["section": feed.id])
                    }
                }
            }
        }
    }

    private func row(title: String, subtitle: String?, key: String, args: [String: String]) -> some View {
        ChoiceRow(title: title, subtitle: subtitle, chosen: taken.contains(key), busy: busy == key) {
            if !taken.contains(key), !full, busy == nil { onPick(key, args) }
        }
    }

    private func matches(_ name: String) -> Bool {
        let q = query.trimmingCharacters(in: .whitespaces)
        return q.isEmpty || name.localizedCaseInsensitiveContains(q)
    }

    // MARK: - Kagi

    private func loadCategories() async {
        struct Index: Decodable {
            struct Category: Decodable { let name: String; let file: String }
            let categories: [Category]
        }
        do {
            let (data, _) = try await URLSession.shared.data(from: URL(string: "https://kite.kagi.com/kite.json")!)
            let index = try JSONDecoder().decode(Index.self, from: data)
            // "OnThisDay" is a different kind of file (history, not news).
            categories = index.categories.filter { $0.file.hasSuffix(".json") && $0.name != "OnThisDay" }
                .map { KagiCategory(name: $0.name, file: $0.file) }
            failed = false
        } catch {
            failed = true
        }
    }
}
