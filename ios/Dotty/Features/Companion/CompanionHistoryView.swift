import SwiftUI

/// Past chats with the companion, kept on Dotty's SD card one file per day
/// (`companion.history`): the days, then a day's exchanges as chat bubbles.
struct CompanionHistoryView: View {
    @Environment(DottyLink.self) private var link
    /// Whose chats (nil = the companion active on Dotty).
    var companion: String? = nil
    @State private var days: [String] = []
    @State private var loaded = false
    @State private var error: String?

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Past chats", subtitle: "Kept on Dotty's SD card, one day at a time.")
                        .padding(.top, Spacing.l)
                    NotConnectedNotice()
                    if let error { NoticeCard(kind: .error, text: error) }
                    GlassCard(title: "Days") {
                        if loaded && days.isEmpty {
                            LightRow(title: "No chats yet", subtitle: "Hold BOOT on Dotty and say hello", systemImage: "bubble.left")
                        }
                        ForEach(days, id: \.self) { day in
                            NavigationLink {
                                CompanionDayView(day: day, companion: companion)
                            } label: {
                                LightRow(title: Self.title(day), systemImage: "calendar") {
                                    Image(systemName: "chevron.right").font(.system(size: 14, weight: .semibold, design: .rounded))
                                }
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    .needsDotty(link)
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await load() }
        }
        .navigationTitle("")
        .task { await load() }
        .onChange(of: link.eventCount) { _, _ in
            if link.lastEvent?.event == "companion.changed" { Task { await load() } }
        }
    }

    /// "Today", "Yesterday" or "Mon 20 Oct" for "2026-10-20".
    static func title(_ day: String) -> String {
        let parse = DateFormatter()
        parse.dateFormat = "yyyy-MM-dd"
        guard let date = parse.date(from: day) else { return day }
        if Calendar.current.isDateInToday(date) { return "Today" }
        if Calendar.current.isDateInYesterday(date) { return "Yesterday" }
        return date.formatted(.dateTime.weekday(.abbreviated).day().month(.abbreviated))
    }

    private func load() async {
        do {
            let r = try await link.send("companion.history", companion.map { ["companion": $0] } ?? [:])
            days = r["days"] as? [String] ?? []
            loaded = true
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}

/// One day's chats: what you said on the right, Dotty's answer on the left.
struct CompanionDayView: View {
    let day: String
    var companion: String? = nil
    @Environment(DottyLink.self) private var link
    @Environment(\.dismiss) private var dismiss
    @State private var turns: [Turn] = []
    @State private var error: String?
    @State private var confirmForget = false

    struct Turn: Identifiable {
        let id: Int
        let time: Date
        let you: String, dotty: String
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.m) {
                    PageHeader(title: CompanionHistoryView.title(day), subtitle: turns.count == 1 ? "1 chat" : "\(turns.count) chats")
                        .padding(.top, Spacing.l)
                    if let error { NoticeCard(kind: .error, text: error) }
                    ForEach(turns) { turn in
                        Text(turn.time.formatted(date: .omitted, time: .shortened))
                            .font(.lpCaption).foregroundStyle(Color.inkFaint)
                            .frame(maxWidth: .infinity)
                        if !turn.you.isEmpty { bubble(turn.you, mine: true) }
                        if !turn.dotty.isEmpty { bubble(turn.dotty, mine: false) }
                    }
                    Button("Forget this day", role: .destructive) { confirmForget = true }
                        .buttonStyle(.quiet())
                        .frame(maxWidth: .infinity)
                        .padding(.top, Spacing.l)
                        .needsDotty(link)
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
        }
        .navigationTitle("")
        .task { await load() }
        .confirmationDialog("Forget this day's chats?", isPresented: $confirmForget, titleVisibility: .visible) {
            Button("Forget", role: .destructive) { Task { await forget() } }
        }
    }

    private var scope: [String: Any] { companion.map { ["companion": $0] } ?? [:] }

    private func bubble(_ text: String, mine: Bool) -> some View {
        HStack {
            if mine { Spacer(minLength: 48) }
            Text(text)
                .font(.lpBody)
                .foregroundStyle(Color.ink)
                .padding(.horizontal, Spacing.m)
                .padding(.vertical, Spacing.s)
                .background(RoundedRectangle(cornerRadius: 18)
                    .fill(mine ? DottyLight.firefly.color.opacity(0.25) : Color.glassStrong))
                .fixedSize(horizontal: false, vertical: true)
            if !mine { Spacer(minLength: 48) }
        }
    }

    private func load() async {
        do {
            let r = try await link.send("companion.history", ["day": day].merging(scope) { a, _ in a })
            let list = r["turns"] as? [[String: Any]] ?? []
            turns = list.enumerated().map { i, t in
                Turn(id: i, time: Date(timeIntervalSince1970: TimeInterval(t["t"] as? Int ?? 0)),
                     you: t["you"] as? String ?? "", dotty: t["dotty"] as? String ?? "")
            }
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func forget() async {
        do {
            try await link.send("companion.forget", ["day": day].merging(scope) { a, _ in a })
            dismiss()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}
