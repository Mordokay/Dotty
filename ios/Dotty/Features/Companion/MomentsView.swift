import SwiftUI

/// Companion › Moments (firmware: cartridges/companion/moments.*): reminders and alarms the companions
/// say by themselves at a set minute, the running timer, and the rules shared by every companion (how
/// many lines of their own a day, the hours for them, quiet windows, the alarm volume, how often a
/// locked Dotty reaches this iPhone). Dotty keeps local time: its times are local epoch seconds.
struct MomentsView: View {
    @Environment(DottyLink.self) private var link
    @State private var moments: [DottyMoment] = []
    @State private var timer: DottyTimer?
    @State private var rules = MomentRulesModel()
    @State private var companions: [CompanionRef] = []
    @State private var activeCompanion = ""
    @State private var available = true
    @State private var loaded = false
    @State private var error: String?
    @State private var showTimerSheet = false
    @State private var editingWindow: Int?
    @State private var addingWindow = false

    struct CompanionRef: Identifiable, Hashable {
        let id: String, name: String, avatar: String
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Moments",
                               subtitle: "Reminders, alarms and timers your companions say by themselves.")
                        .padding(.top, Spacing.l)
                    NotConnectedNotice()
                    if let error { NoticeCard(kind: .error, text: error) }
                    if !available { NoticeCard(kind: .info, text: "Dotty needs its SD card for moments. A timer still works.") }
                    timerCard
                    momentSections
                    rulesCard
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await load() }
        }
        .navigationTitle("")
        .task { await load() }
        .onChange(of: link.eventCount) { _, _ in
            let e = link.lastEvent?.event
            if e == "companion.moments" || e == "companion.timer" { Task { await load() } }
        }
        .sheet(isPresented: $showTimerSheet) { TimerSheet(running: timer != nil, onStarted: { Task { await load() } }) }
        .sheet(isPresented: $addingWindow) {
            QuietWindowSheet(window: MomentRulesModel.Window()) { w in
                rules.quiet.append(w)
                Task { await saveRules(["quiet": rules.quietJSON]) }
            }
        }
        .sheet(item: Binding(get: { editingWindow.map { IdentifiedIndex(id: $0) } }, set: { editingWindow = $0?.id })) { i in
            QuietWindowSheet(window: rules.quiet[i.id], onDelete: {
                rules.quiet.remove(at: i.id)
                Task { await saveRules(["quiet": rules.quietJSON]) }
            }) { w in
                rules.quiet[i.id] = w
                Task { await saveRules(["quiet": rules.quietJSON]) }
            }
        }
    }

    // MARK: - The timer

    @ViewBuilder private var timerCard: some View {
        GlassCard(title: "Timer") {
            if let timer {
                HStack(spacing: Spacing.m) {
                    Image(systemName: "hourglass").font(.system(size: 22, weight: .semibold)).foregroundStyle(Color.ink)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(timer.label.isEmpty ? "Timer" : timer.label.capitalized).font(.lpHeadline).foregroundStyle(Color.ink)
                        if timer.paused {
                            Text("Paused · \(Self.clock(timer.left)) left").font(.lpCaption).foregroundStyle(Color.inkMuted)
                        } else {
                            Text(timerInterval: Date()...max(Date(), timer.ends), countsDown: true)
                                .font(.system(size: 28, weight: .bold, design: .rounded).monospacedDigit())
                                .foregroundStyle(Color.ink)
                        }
                    }
                    Spacer()
                    Button(timer.paused ? "Resume" : "Pause") {
                        Task { await timerAction(timer.paused ? "resume" : "pause") }
                    }
                    .buttonStyle(.quiet())
                    Button("Cancel") { Task { await timerAction("cancel") } }
                        .buttonStyle(.quiet(DottyLight.ember.color))
                }
                .padding(Spacing.m)
            } else {
                LightRow(title: "Start a timer", subtitle: "Dotty beeps the last ten seconds, then rings",
                         systemImage: "hourglass", action: { showTimerSheet = true })
            }
        }
        .needsDotty(link)
    }

    // MARK: - The moments

    private var upcoming: [DottyMoment] { moments.filter { $0.state == "upcoming" } }
    private var today: [DottyMoment] { upcoming.filter { $0.repeat == "once" && Calendar.current.isDateInToday($0.date) } }
    private var later: [DottyMoment] { upcoming.filter { $0.repeat == "once" && !Calendar.current.isDateInToday($0.date) } }
    private var repeating: [DottyMoment] { upcoming.filter { $0.repeat != "once" } }
    private var missed: [DottyMoment] { moments.filter { $0.state == "missed" } }

    @ViewBuilder private var momentSections: some View {
        if loaded && upcoming.isEmpty && missed.isEmpty {
            GlassCard {
                LightRow(title: "Nothing set yet", subtitle: "Ask a companion (\u{201C}remind me at 6 to cook dinner\u{201D}) or add one here",
                         systemImage: "bell")
            }
        }
        section("Today", today)
        section("Upcoming", later)
        section("Repeating", repeating)
        section("Missed", missed)
        NavigationLink {
            MomentEditor(moment: DottyMoment.new(companion: activeCompanion), companions: companions,
                         onSaved: { Task { await load() } })
        } label: {
            Label("New reminder or alarm", systemImage: "plus")
        }
        .buttonStyle(.light())
        .needsDotty(link)
        .disabled(!available)
    }

    @ViewBuilder private func section(_ title: String, _ items: [DottyMoment]) -> some View {
        if !items.isEmpty {
            GlassCard(title: title) {
                ForEach(items) { m in row(m) }
            }
            .needsDotty(link)
        }
    }

    private func row(_ m: DottyMoment) -> some View {
        HStack(spacing: Spacing.s) {
            NavigationLink {
                MomentEditor(moment: m, companions: companions, onSaved: { Task { await load() } })
            } label: {
                LightRow(title: m.what, subtitle: "\(m.whenText) · \(m.statusText)", systemImage: m.icon) {
                    if let c = companions.first(where: { $0.id == m.companion }) {
                        CompanionView.avatarImage(c.avatar, companion: c.id, size: 30)
                    }
                }
            }
            .buttonStyle(.plain)
            .disabled(m.kind == "spontaneous")
            Menu {
                Button { Task { await play(m) } } label: { Label("Play on Dotty", systemImage: "play") }
                Button(role: .destructive) { Task { await delete(m) } } label: { Label("Delete", systemImage: "trash") }
            } label: {
                Image(systemName: "ellipsis.circle").font(.system(size: 20)).foregroundStyle(Color.inkMuted)
                    .padding(.trailing, Spacing.m)
            }
        }
    }

    // MARK: - The rules

    @ViewBuilder private var rulesCard: some View {
        GlassCard(title: "Rules for every companion") {
            VStack(alignment: .leading, spacing: Spacing.m) {
                Stepper(value: Binding(get: { rules.perDay }, set: { v in
                    rules.perDay = v
                    Task { await saveRules(["perDay": v]) }
                }), in: 0...5) {
                    VStack(alignment: .leading, spacing: 0) {
                        Text("Their own lines a day: \(rules.perDay)").font(.lpHeadline).foregroundStyle(Color.ink)
                        Text("A joke, a question, \u{201C}I miss you\u{201D}…").font(.lpCaption).foregroundStyle(Color.inkMuted)
                    }
                }
                HStack {
                    Text("Between").font(.lpBody).foregroundStyle(Color.ink)
                    DatePicker("", selection: minutesBinding(\.from, key: "from"), displayedComponents: .hourAndMinute).labelsHidden()
                    Text("and").font(.lpBody).foregroundStyle(Color.ink)
                    DatePicker("", selection: minutesBinding(\.to, key: "to"), displayedComponents: .hourAndMinute).labelsHidden()
                }
                Divider()
                Text("Quiet windows").font(.lpHeadline).foregroundStyle(Color.ink)
                Text("Work hours, do-not-disturb: the companions' own lines never come then.")
                    .font(.lpCaption).foregroundStyle(Color.inkMuted)
                ForEach(Array(rules.quiet.enumerated()), id: \.offset) { i, w in
                    LightRow(title: w.daysText, subtitle: "\(Self.minutesText(w.from)) – \(Self.minutesText(w.to))",
                             systemImage: "moon", action: { editingWindow = i })
                }
                Button { addingWindow = true } label: { Label("Add a quiet window", systemImage: "plus") }
                    .buttonStyle(.quiet())
                Text("A reminder in a quiet window").font(.lpLabel).foregroundStyle(Color.inkMuted)
                ChoiceRow(title: "Beeps and text", systemImage: "bell", chosen: !rules.quietSilent) {
                    rules.quietSilent = false
                    Task { await saveRules(["quietMode": "beeps"]) }
                } trailing: { EmptyView() }
                ChoiceRow(title: "Silent, text only", systemImage: "bell.slash", chosen: rules.quietSilent) {
                    rules.quietSilent = true
                    Task { await saveRules(["quietMode": "silent"]) }
                } trailing: { EmptyView() }
                Divider()
                VStack(alignment: .leading, spacing: 2) {
                    Text("Alarm volume: \(rules.alarmVolume)%").font(.lpHeadline).foregroundStyle(Color.ink)
                    Slider(value: Binding(get: { Double(rules.alarmVolume) }, set: { rules.alarmVolume = Int($0) }),
                           in: 80...100, step: 5) { editing in
                        if !editing { Task { await saveRules(["alarmVolume": rules.alarmVolume]) } }
                    }
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text("A locked Dotty checks in with this iPhone").font(.lpHeadline).foregroundStyle(Color.ink)
                    Picker("", selection: Binding(get: { rules.wakeEvery }, set: { v in
                        rules.wakeEvery = v
                        Task { await saveRules(["wakeEvery": v]) }
                    })) {
                        ForEach([10, 20, 30, 60], id: \.self) { Text("every \($0) min").tag($0) }
                    }
                    .pickerStyle(.segmented)
                    Text("Only when it has lines to prepare. More often reacts sooner; less often saves battery.")
                        .font(.lpCaption).foregroundStyle(Color.inkMuted)
                }
            }
            .padding(Spacing.m)
        }
        .needsDotty(link)
        .disabled(!available)
    }

    private func minutesBinding(_ path: WritableKeyPath<MomentRulesModel, Int>, key: String) -> Binding<Date> {
        Binding(get: { Self.date(minutes: rules[keyPath: path]) }, set: { d in
            let c = Calendar.current.dateComponents([.hour, .minute], from: d)
            let v = (c.hour ?? 0) * 60 + (c.minute ?? 0)
            rules[keyPath: path] = v
            Task { await saveRules([key: v]) }
        })
    }

    // MARK: - Commands

    private func load() async {
        do {
            let r = try await link.send("companion.moments")
            available = r["available"] as? Bool ?? true
            moments = (r["moments"] as? [[String: Any]] ?? []).compactMap(DottyMoment.init).sorted { $0.at < $1.at }
            timer = (r["timer"] as? [String: Any]).flatMap(DottyTimer.init)
            let rr = try await link.send("companion.moment.rules")
            if let o = rr["rules"] as? [String: Any] { rules = MomentRulesModel(o) }
            let list = try await link.send("companion.list")
            companions = (list["companions"] as? [[String: Any]] ?? []).map {
                CompanionRef(id: $0["id"] as? String ?? "", name: $0["name"] as? String ?? "", avatar: $0["avatar"] as? String ?? "robot")
            }
            activeCompanion = list["active"] as? String ?? companions.first?.id ?? ""
            loaded = true
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func saveRules(_ fields: [String: Any]) async {
        do {
            let r = try await link.send("companion.moment.rules", fields)
            if let o = r["rules"] as? [String: Any] { rules = MomentRulesModel(o) }
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func timerAction(_ action: String) async {
        do {
            _ = try await link.send("companion.timer", ["action": action])
            await load()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func play(_ m: DottyMoment) async {
        do {
            _ = try await link.send("companion.moment.play", ["companion": m.companion, "id": m.momentId])
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func delete(_ m: DottyMoment) async {
        do {
            _ = try await link.send("companion.moment.delete", ["companion": m.companion, "id": m.momentId])
            await load()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    // MARK: - Times

    static func clock(_ seconds: Int) -> String {
        seconds >= 3600 ? String(format: "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60)
                        : String(format: "%d:%02d", seconds / 60, seconds % 60)
    }

    static func minutesText(_ minutes: Int) -> String { String(format: "%02d:%02d", minutes / 60 % 24, minutes % 60) }

    static func date(minutes: Int) -> Date {
        Calendar.current.date(bySettingHour: minutes / 60 % 24, minute: minutes % 60, second: 0, of: Date()) ?? Date()
    }
}

private struct IdentifiedIndex: Identifiable {
    let id: Int
}

// MARK: - Models

/// Dotty's clock is local: its epoch seconds already include the zone offset.
enum DottyTime {
    static func date(_ local: Int) -> Date {
        let guess = Date(timeIntervalSince1970: TimeInterval(local))
        return guess.addingTimeInterval(-TimeInterval(TimeZone.current.secondsFromGMT(for: guess)))
    }

    static func local(_ date: Date) -> Int {
        Int(date.timeIntervalSince1970) + TimeZone.current.secondsFromGMT(for: date)
    }
}

struct DottyMoment: Identifiable, Hashable {
    var id: String { "\(companion)-\(momentId)" }
    var momentId: Int
    var companion: String
    var kind: String
    var at: Int
    var `repeat`: String
    var days: [String]
    var what: String
    var style: String
    var source: String
    var ready: Bool
    var state: String

    var date: Date { DottyTime.date(at) }

    static func new(companion: String) -> DottyMoment {
        let inAnHour = Date().addingTimeInterval(3600)
        return DottyMoment(momentId: 0, companion: companion, kind: "reminder", at: DottyTime.local(inAnHour), repeat: "once",
                           days: [], what: "", style: "spoken", source: "app", ready: false, state: "upcoming")
    }

    init(momentId: Int, companion: String, kind: String, at: Int, repeat: String, days: [String], what: String,
         style: String, source: String, ready: Bool, state: String) {
        self.momentId = momentId
        self.companion = companion
        self.kind = kind
        self.at = at
        self.repeat = `repeat`
        self.days = days
        self.what = what
        self.style = style
        self.source = source
        self.ready = ready
        self.state = state
    }

    init?(_ o: [String: Any]) {
        guard let id = o["id"] as? Int else { return nil }
        self.init(momentId: id, companion: o["companion"] as? String ?? "", kind: o["kind"] as? String ?? "reminder",
                  at: o["at"] as? Int ?? 0, repeat: o["repeat"] as? String ?? "once", days: o["days"] as? [String] ?? [],
                  what: o["what"] as? String ?? "", style: o["style"] as? String ?? "spoken",
                  source: o["source"] as? String ?? "app", ready: o["ready"] as? Bool ?? false,
                  state: o["state"] as? String ?? "upcoming")
    }

    var icon: String { kind == "alarm" ? "alarm" : kind == "spontaneous" ? "bubble.left" : "bell" }

    var whenText: String {
        let time = date.formatted(date: .omitted, time: .shortened)
        switch `repeat` {
        case "daily": return "Daily \(time)"
        case "weekdays": return "Weekdays \(time)"
        case "days": return days.map { $0.prefix(1).uppercased() + $0.dropFirst() }.joined(separator: ", ") + " \(time)"
        default:
            if Calendar.current.isDateInToday(date) { return "Today \(time)" }
            if Calendar.current.isDateInTomorrow(date) { return "Tomorrow \(time)" }
            return date.formatted(.dateTime.weekday(.abbreviated).day().month(.abbreviated).hour().minute())
        }
    }

    var statusText: String {
        if state == "missed" { return "Missed" }
        if kind == "spontaneous" { return ready ? "Its own line, ready" : "Its own line" }
        return ready ? "Recorded" : "Records at the next chat"
    }
}

struct DottyTimer: Hashable {
    var label: String
    var endsAt: Int
    var paused: Bool
    var left: Int
    var ends: Date { DottyTime.date(endsAt) }

    init?(_ o: [String: Any]) {
        label = o["label"] as? String ?? ""
        endsAt = o["endsAt"] as? Int ?? 0
        paused = o["paused"] as? Bool ?? false
        left = o["left"] as? Int ?? 0
    }
}

struct MomentRulesModel: Hashable {
    struct Window: Hashable {
        var days = 0x1F  // bit 0 = Monday … bit 6 = Sunday
        var from = 9 * 60, to = 17 * 60

        static let names = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
        var daysText: String {
            if days == 0x7F { return "Every day" }
            if days == 0x1F { return "Weekdays" }
            if days == 0x60 { return "Weekends" }
            return (0..<7).filter { days & (1 << $0) != 0 }.map { Self.names[$0] }.joined(separator: ", ")
        }
    }

    var perDay = 2, from = 600, to = 1260
    var quiet: [Window] = []
    var quietSilent = false
    var alarmVolume = 90, wakeEvery = 20

    init() {}

    init(_ o: [String: Any]) {
        perDay = o["perDay"] as? Int ?? 2
        from = o["from"] as? Int ?? 600
        to = o["to"] as? Int ?? 1260
        quiet = (o["quiet"] as? [[String: Any]] ?? []).map {
            Window(days: $0["days"] as? Int ?? 0x7F, from: $0["from"] as? Int ?? 0, to: $0["to"] as? Int ?? 0)
        }
        quietSilent = (o["quietMode"] as? String) == "silent"
        alarmVolume = o["alarmVolume"] as? Int ?? 90
        wakeEvery = o["wakeEvery"] as? Int ?? 20
    }

    var quietJSON: [[String: Int]] { quiet.map { ["days": $0.days, "from": $0.from, "to": $0.to] } }
}

// MARK: - Sheets

/// Start a timer: hours, minutes, seconds and what it's for.
private struct TimerSheet: View {
    let running: Bool
    let onStarted: () -> Void
    @Environment(DottyLink.self) private var link
    @Environment(\.dismiss) private var dismiss
    @State private var hours = 0
    @State private var minutes = 10
    @State private var seconds = 0
    @State private var label = ""
    @State private var confirmReplace = false
    @State private var error: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    HStack(spacing: 0) {
                        wheel($hours, 0..<24, "h")
                        wheel($minutes, 0..<60, "min")
                        wheel($seconds, 0..<60, "s")
                    }
                    .frame(height: 150)
                }
                Section("What it's for") { TextField("Pasta", text: $label) }
                if let error { Section { Text(error).foregroundStyle(.red) } }
            }
            .navigationTitle("Timer")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Start") { if running { confirmReplace = true } else { Task { await start() } } }
                        .disabled(hours * 3600 + minutes * 60 + seconds == 0)
                }
            }
            .confirmationDialog("Replace the running timer?", isPresented: $confirmReplace, titleVisibility: .visible) {
                Button("Replace") { Task { await start(replacing: true) } }
            }
        }
        .presentationDetents([.medium])
    }

    private func wheel(_ value: Binding<Int>, _ range: Range<Int>, _ unit: String) -> some View {
        Picker(unit, selection: value) {
            ForEach(Array(range), id: \.self) { Text("\($0) \(unit)").tag($0) }
        }
        .pickerStyle(.wheel)
    }

    private func start(replacing: Bool = false) async {
        do {
            if replacing { _ = try await link.send("companion.timer", ["action": "cancel"]) }
            _ = try await link.send("companion.timer", ["action": "start", "seconds": hours * 3600 + minutes * 60 + seconds,
                                                        "label": label.trimmingCharacters(in: .whitespaces)])
            onStarted()
            dismiss()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}

/// A quiet window: which days, from when to when (past midnight is fine).
private struct QuietWindowSheet: View {
    @State var window: MomentRulesModel.Window
    var onDelete: (() -> Void)?
    let onSave: (MomentRulesModel.Window) -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section("Days") {
                    HStack {
                        ForEach(0..<7, id: \.self) { i in
                            let on = window.days & (1 << i) != 0
                            Button(MomentRulesModel.Window.names[i].prefix(2)) { window.days ^= 1 << i }
                                .buttonStyle(.borderedProminent)
                                .tint(on ? DottyLight.firefly.color : Color.gray.opacity(0.3))
                                .font(.caption)
                        }
                    }
                }
                Section("Hours") {
                    DatePicker("From", selection: binding(\.from), displayedComponents: .hourAndMinute)
                    DatePicker("To", selection: binding(\.to), displayedComponents: .hourAndMinute)
                }
                if let onDelete {
                    Section { Button("Delete", role: .destructive) { onDelete(); dismiss() } }
                }
            }
            .navigationTitle("Quiet window")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { onSave(window); dismiss() }.disabled(window.days == 0 || window.from == window.to)
                }
            }
        }
    }

    private func binding(_ path: WritableKeyPath<MomentRulesModel.Window, Int>) -> Binding<Date> {
        Binding(get: { MomentsView.date(minutes: window[keyPath: path]) }, set: { d in
            let c = Calendar.current.dateComponents([.hour, .minute], from: d)
            window[keyPath: path] = (c.hour ?? 0) * 60 + (c.minute ?? 0)
        })
    }
}
