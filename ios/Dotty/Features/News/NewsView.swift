import SwiftUI

/// The News cartridge's screen: the topics Dotty follows — outlets' feeds, Kagi News daily
/// briefings, Google News keywords — which are favourites (★: Dotty's Favourites list and lock
/// screen), when it last fetched, and what Dotty is showing now (firmware: cartridges/news/).
struct NewsView: View {
    @Environment(DottyLink.self) private var link
    @State private var status: Status?
    @State private var sections: [Section] = []
    @State private var stories: [Story] = []
    @State private var keyword = ""
    @State private var busy: String?  // the topic key (or "add") being changed
    @State private var picking: TopicPicker.Kind?
    @State private var error: String?
    @FocusState private var keywordFocused: Bool

    static let maxTopics = 12

    struct Status {
        var stories = 0
        var fetching = false
        var done = 0
        var total = 0
        var fetchedAt: Date?
        var error: String?
        var topics: [Topic] = []
    }

    struct Topic: Identifiable {
        var id: String { key }
        let key: String
        let name: String
        let section: String?
        let kagi: String?
        let star: Bool
        let count: Int
    }

    /// An outlet's feed Dotty can follow ("NYT" + "World").
    struct Section: Identifiable {
        let id: String
        let outlet: String
        let section: String
        let name: String
    }

    struct Story: Identifiable {
        var id: String { title }
        let title: String
        let source: String
        let summary: String
        let published: Date?
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "News", subtitle: "Short, recent stories for your topics. Dotty fetches them every 30 minutes and keeps them to read offline.")
                        .padding(.top, Spacing.l)
                    NotConnectedNotice()
                    if let error { NoticeCard(kind: .error, text: error) }
                    if let status {
                        statusCard(status).needsDotty(link)
                        topicsCard(status).needsDotty(link)
                        addCard(status).needsDotty(link)
                    }
                    if !stories.isEmpty { storiesCard }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .scrollDismissesKeyboard(.interactively)
            .refreshable { await load() }
        }
        .navigationTitle("")
        .task { await load() }
        .onChange(of: link.connection) { _, state in
            if state == .connected { Task { await load() } }
        }
        .onChange(of: link.eventCount) { _, _ in
            if link.lastEvent?.event == "news.changed" { Task { await load() } }
        }
        // While Dotty fetches, follow its progress.
        .sheet(item: $picking) { kind in
            TopicPicker(kind: kind, outlets: sections, taken: Set(status?.topics.map(\.key) ?? []), busy: busy,
                        full: (status?.topics.count ?? 0) >= Self.maxTopics) { key, args in
                Task { await change(key, "news.topic.add", args) }
            }
            .presentationDetents([.large])
            .presentationBackground(.clear)
        }
        .task(id: status?.fetching == true) {
            while status?.fetching == true, !Task.isCancelled {
                try? await Task.sleep(for: .seconds(2))
                await loadStatus()
            }
        }
    }

    // MARK: - Cards

    private func statusCard(_ status: Status) -> some View {
        GlassCard(title: status.stories == 0 ? "No stories yet" : "\(status.stories) stories on Dotty") {
            if status.fetching {
                VStack(alignment: .leading, spacing: Spacing.s) {
                    LightProgress(value: status.total > 0 ? Double(status.done) / Double(status.total) : 0)
                    Text("Dotty is reading \(status.done + 1 > status.total ? status.total : status.done + 1) of \(status.total) topics")
                        .font(.lpCaption).foregroundStyle(Color.inkMuted)
                }
                .padding(Spacing.m)
            } else {
                LightRow(title: "Refresh now", subtitle: updatedLine(status), systemImage: "arrow.clockwise",
                         action: { Task { await send("news.refresh") } })
            }
            if let problem = status.error, !status.fetching {
                Text("Last try: \(problem)").font(.lpCaption).foregroundStyle(DottyLight.ember.color)
                    .padding(.horizontal, Spacing.m).padding(.bottom, Spacing.s)
            }
        }
    }

    private func updatedLine(_ status: Status) -> String {
        guard let date = status.fetchedAt else { return "Every 30 minutes, also while Dotty is locked" }
        return "Updated \(date.formatted(.relative(presentation: .named))) · every 30 minutes"
    }

    private func topicsCard(_ status: Status) -> some View {
        GlassCard(title: "Your topics (\(status.topics.count)/\(Self.maxTopics))") {
            ForEach(Array(status.topics.enumerated()), id: \.element.key) { index, topic in
                LightRow(title: topic.name, subtitle: source(of: topic) + " · \(topic.count) stories",
                         systemImage: topic.kagi != nil ? "sparkles.rectangle.stack" : topic.section != nil ? "newspaper" : "magnifyingglass") {
                    HStack(spacing: Spacing.xs) {
                        Button {
                            Task { await change(topic.key, "news.topic.star", ["key": topic.key, "on": !topic.star]) }
                        } label: {
                            Image(systemName: topic.star ? "star.fill" : "star")
                                .font(.system(size: 20, weight: .semibold))
                                .foregroundStyle(topic.star ? DottyLight.firefly.color : Color.inkMuted)
                                .frame(width: 36, height: 36)
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(topic.star ? "Remove from favourites" : "Add to favourites")
                        Menu {
                            if index > 0 {
                                Button("Move up", systemImage: "arrow.up") {
                                    Task { await change(topic.key, "news.topic.move", ["from": index, "to": index - 1]) }
                                }
                            }
                            if index < status.topics.count - 1 {
                                Button("Move down", systemImage: "arrow.down") {
                                    Task { await change(topic.key, "news.topic.move", ["from": index, "to": index + 1]) }
                                }
                            }
                            Button("Remove", systemImage: "trash", role: .destructive) {
                                Task { await change(topic.key, "news.topic.remove", ["key": topic.key]) }
                            }
                        } label: {
                            Image(systemName: "ellipsis.circle")
                                .font(.system(size: 20, weight: .semibold))
                                .frame(width: 36, height: 36)
                                .contentShape(Rectangle())
                        }
                        .foregroundStyle(Color.inkMuted)
                    }
                    .opacity(busy == topic.key ? 0.4 : 1)
                }
            }
            Text("★ topics make up Dotty's Favourites and its lock screen.")
                .font(.lpCaption).foregroundStyle(Color.inkFaint)
                .padding(.horizontal, Spacing.m).padding(.bottom, Spacing.s)
        }
    }

    private func addCard(_ status: Status) -> some View {
        GlassCard(title: "Add a topic") {
            LightRow(title: "Daily briefing", subtitle: "The day's big stories from dozens of outlets, summarised · Kagi News",
                     systemImage: "sparkles.rectangle.stack", action: { picking = .briefing })
            LightRow(title: "News outlet", subtitle: "NYT, NBC, BBC, Bloomberg, Guardian… fresh all day",
                     systemImage: "newspaper", action: { picking = .outlet })
            if status.topics.count < Self.maxTopics {
                HStack(spacing: Spacing.s) {
                    TextField("Add a keyword, e.g. Formula 1", text: $keyword)
                        .font(.lpBody)
                        .foregroundStyle(Color.ink)
                        .focused($keywordFocused)
                        .submitLabel(.done)
                        .onSubmit { Task { await addKeyword() } }
                        .padding(.horizontal, Spacing.m)
                        .frame(height: 44)
                        .background(RoundedRectangle(cornerRadius: 12).fill(Color.glassStrong))
                    Button("Add") { Task { await addKeyword() } }
                        .buttonStyle(.light())
                        .disabled(keyword.trimmingCharacters(in: .whitespaces).isEmpty || busy != nil)
                }
                .padding(Spacing.m)
            }
            Text("Keywords search Google News over the last 2 days: headlines only, no summary. Try \"site:wsj.com\" for an outlet without a feed.")
                .font(.lpCaption).foregroundStyle(Color.inkFaint)
                .padding(.horizontal, Spacing.m).padding(.bottom, Spacing.s)
        }
    }

    /// Where a topic's stories come from.
    private func source(of topic: Topic) -> String {
        if topic.kagi != nil { return "Daily briefing · Kagi News" }
        if let id = topic.section { return sections.first { $0.id == id }?.outlet ?? "News outlet" }
        return "Keyword · headlines only"
    }

    private var storiesCard: some View {
        GlassCard(title: "Favourites on Dotty now") {
            ForEach(stories) { story in
                VStack(alignment: .leading, spacing: Spacing.xs) {
                    Text(story.title)
                        .font(.lpHeadline).foregroundStyle(Color.ink)
                        .fixedSize(horizontal: false, vertical: true)
                    Text(storyLine(story)).font(.lpCaption).foregroundStyle(Color.inkMuted)
                    if !story.summary.isEmpty {
                        Text(story.summary)
                            .font(.lpBody).foregroundStyle(Color.inkMuted)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(Spacing.m)
            }
        }
    }

    private func storyLine(_ story: Story) -> String {
        guard let date = story.published else { return story.source }
        return "\(story.source) · \(date.formatted(.relative(presentation: .named)))"
    }

    // MARK: - Dotty

    private func load() async {
        await loadStatus()
        if sections.isEmpty, let reply = try? await link.send("news.sections") {
            sections = (reply["sections"] as? [[String: Any]] ?? []).compactMap { item in
                guard let id = item["id"] as? String, let name = item["name"] as? String else { return nil }
                return Section(id: id, outlet: item["outlet"] as? String ?? "BBC", section: item["section"] as? String ?? name,
                               name: name)
            }
        }
        if let reply = try? await link.send("news.stories", ["limit": 8]) {
            stories = (reply["stories"] as? [[String: Any]] ?? []).map { item in
                let published = (item["published"] as? Double).flatMap { $0 > 1_600_000_000 ? Date(timeIntervalSince1970: $0) : nil }
                return Story(title: item["title"] as? String ?? "", source: item["source"] as? String ?? "",
                             summary: item["summary"] as? String ?? "", published: published)
            }
        }
    }

    private func loadStatus() async {
        do {
            let reply = try await link.send("news.status")
            var s = Status()
            s.stories = reply["stories"] as? Int ?? 0
            s.fetching = reply["fetching"] as? Bool ?? false
            s.done = reply["done"] as? Int ?? 0
            s.total = reply["total"] as? Int ?? 0
            // Dotty keeps local time: its epoch seconds are the local clock's reading.
            if let at = reply["fetchedAt"] as? Double, at > 1_600_000_000 {
                s.fetchedAt = Date(timeIntervalSince1970: at - Double(TimeZone.current.secondsFromGMT()))
            }
            s.error = reply["error"] as? String
            s.topics = (reply["topics"] as? [[String: Any]] ?? []).compactMap { item in
                guard let key = item["key"] as? String else { return nil }
                return Topic(key: key, name: item["name"] as? String ?? key, section: item["section"] as? String,
                             kagi: item["kagi"] as? String, star: item["star"] as? Bool ?? false,
                             count: item["count"] as? Int ?? 0)
            }
            status = s
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func addKeyword() async {
        let query = keyword.trimmingCharacters(in: .whitespaces)
        guard !query.isEmpty else { return }
        await change("k:" + query, "news.topic.add", ["query": query])
        if error == nil {
            keyword = ""
            keywordFocused = false
        }
    }

    private func change(_ key: String, _ command: String, _ args: [String: Any]) async {
        busy = key
        defer { busy = nil }
        await send(command, args)
    }

    private func send(_ command: String, _ args: [String: Any] = [:]) async {
        do {
            try await link.send(command, args)
            error = nil
            await loadStatus()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}
