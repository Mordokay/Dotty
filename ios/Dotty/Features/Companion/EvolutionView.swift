import SwiftUI

/// How a companion has changed: after a chat that really changed it, Dotty rewrites one field of its
/// character (`companion.evolution`, newest first) and logs what and why. Undo puts the field back as
/// it was (`companion.evolution.undo`).
struct EvolutionView: View {
    let companion: String
    let name: String
    let onUndo: () -> Void
    @Environment(DottyLink.self) private var link
    @State private var changes: [Change] = []
    @State private var loaded = false
    @State private var undoing: Int?
    @State private var error: String?

    struct Change: Identifiable {
        let t: Int
        let field: String, before: String, after: String, why: String
        let undone: Bool
        var id: Int { t }
    }

    static let labels = ["personality": "Personality", "traits": "Traits", "quirks": "Quirks",
                         "speaking": "Way of speaking", "backstory": "Backstory"]

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "How \(name) has changed",
                               subtitle: "Most chats change nothing. Now and then one does, a little.")
                        .padding(.top, Spacing.l)
                    NotConnectedNotice()
                    if let error { NoticeCard(kind: .error, text: error) }
                    if loaded && changes.isEmpty {
                        GlassCard {
                            LightRow(title: "No changes yet", subtitle: "Pinned lines never change by themselves",
                                     systemImage: "leaf")
                        }
                    }
                    ForEach(changes) { change in card(change) }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await load() }
        }
        .navigationTitle("")
        .task { await load() }
    }

    private func card(_ change: Change) -> some View {
        GlassCard {
            VStack(alignment: .leading, spacing: Spacing.s) {
                HStack {
                    Text(Self.labels[change.field] ?? change.field).font(.lpLabel).foregroundStyle(Color.ink)
                    Spacer()
                    Text(Self.date(change.t)).font(.lpCaption).foregroundStyle(Color.inkFaint)
                }
                if !change.why.isEmpty {
                    Text(change.why).font(.lpCallout).italic().foregroundStyle(Color.inkMuted)
                }
                ForEach(Array(diff(change).enumerated()), id: \.offset) { _, line in
                    HStack(alignment: .firstTextBaseline, spacing: 6) {
                        Text(line.added ? "+" : "−").font(.lpLabel)
                        Text(line.text).font(.lpBody).strikethrough(!line.added)
                    }
                    .foregroundStyle(line.added ? DottyLight.leaf.color : DottyLight.ember.color)
                }
                HStack {
                    Spacer()
                    if change.undone {
                        Text("Undone").font(.lpCaption).foregroundStyle(Color.inkFaint)
                    } else if undoing == change.t {
                        ProgressView().controlSize(.small)
                    } else {
                        Button("Undo") { Task { await undo(change) } }
                            .buttonStyle(.quiet())
                            .disabled(undoing != nil)
                    }
                }
            }
            .padding(Spacing.m)
            .opacity(change.undone ? 0.6 : 1)
        }
        .needsDotty(link)
    }

    /// Removed lines, then added ones (the lines both versions share are left out).
    private func diff(_ change: Change) -> [(text: String, added: Bool)] {
        let before = change.before.split(separator: "\n").map(String.init)
        let after = change.after.split(separator: "\n").map(String.init)
        return before.filter { !after.contains($0) }.map { ($0, false) } + after.filter { !before.contains($0) }.map { ($0, true) }
    }

    /// Dotty's times are local clock seconds.
    static func date(_ t: Int) -> String {
        let date = Date(timeIntervalSince1970: TimeInterval(t - TimeZone.current.secondsFromGMT()))
        return date.formatted(.dateTime.day().month(.abbreviated).hour().minute())
    }

    private func load() async {
        do {
            let r = try await link.send("companion.evolution", ["id": companion])
            changes = (r["changes"] as? [[String: Any]] ?? []).map { c in
                Change(t: c["t"] as? Int ?? 0, field: c["field"] as? String ?? "", before: c["before"] as? String ?? "",
                       after: c["after"] as? String ?? "", why: c["why"] as? String ?? "", undone: c["undone"] as? Bool ?? false)
            }
            loaded = true
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func undo(_ change: Change) async {
        undoing = change.t
        defer { undoing = nil }
        do {
            try await link.send("companion.evolution.undo", ["id": companion, "t": change.t])
            await load()
            onUndo()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}
