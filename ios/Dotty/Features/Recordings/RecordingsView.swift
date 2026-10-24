import SwiftUI

/// A companion's voice notes (spec: docs/superpowers/specs/2026-10-24-companion-recordings-design.md):
/// Dotty's list merged with this iPhone's copies, newest first.
struct RecordingsView: View {
    let companion: String
    let name: String
    @Environment(DottyLink.self) private var link
    @State private var rows: [Row] = []
    @State private var error: String?

    struct Row: Identifiable, Hashable {
        var id: String
        var title: String
        var created: Date
        var seconds: Double
        var summary: String
        var onIPhone: Bool
        var isDraft: Bool
        var onDotty: Bool
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Recordings", subtitle: "Voice notes you recorded with \(name). Ask on Dotty: \u{201C}I want to record something\u{201D}.")
                        .padding(.top, Spacing.l)
                    NotConnectedNotice()
                    if let error { NoticeCard(kind: .error, text: error) }
                    GlassCard {
                        if rows.isEmpty {
                            LightRow(title: "No recordings yet", subtitle: "Say \u{201C}record something about…\u{201D} to \(name)",
                                     systemImage: "waveform")
                        }
                        ForEach(rows) { r in
                            NavigationLink { RecordingDetailView(companion: companion, id: r.id, onChanged: { Task { await load() } }) } label: {
                                LightRow(title: r.title, subtitle: subtitle(r), systemImage: "waveform") {
                                    Image(systemName: "chevron.right").font(.system(size: 14, weight: .semibold, design: .rounded))
                                }
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await load() }
        }
        .navigationTitle("")
        .task { await load() }
        .onChange(of: link.eventCount) { _, _ in
            if link.lastEvent?.event == "companion.recordings" { Task { await load() } }
        }
    }

    private func subtitle(_ r: Row) -> String {
        let when = r.created.formatted(.dateTime.day().month(.abbreviated).hour().minute())
        let length = String(format: "%d:%02d", Int(r.seconds) / 60, Int(r.seconds) % 60)
        var parts = [when, length]
        parts.append(r.onIPhone ? "On iPhone \u{2713}" : "Waiting to sync")
        if r.isDraft { parts.append("draft") }
        if !r.summary.isEmpty { parts.append(r.summary) }
        return parts.joined(separator: " · ")
    }

    private func load() async {
        var byId: [String: Row] = [:]
        for l in RecordingStore.recordings(companion: companion) {
            byId[l.id] = Row(id: l.id, title: l.title, created: l.created, seconds: l.duration, summary: l.summary,
                             onIPhone: true, isDraft: l.final.isEmpty && !l.draft.isEmpty, onDotty: false)
        }
        do {
            let r = try await link.send("companion.recordings", ["companion": companion])
            for item in r["recordings"] as? [[String: Any]] ?? [] {
                guard let id = item["name"] as? String else { continue }
                var row = byId[id] ?? Row(id: id, title: item["title"] as? String ?? id, created: RecordingStore.date(fromName: id),
                                          seconds: Double(item["ms"] as? Int ?? 0) / 1000, summary: item["summary"] as? String ?? "",
                                          onIPhone: false, isDraft: item["isDraft"] as? Bool ?? false, onDotty: true)
                row.onDotty = true
                if let title = item["title"] as? String, !title.isEmpty { row.title = title }
                byId[id] = row
            }
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
        rows = byId.values.sorted { $0.created > $1.created }
    }
}
