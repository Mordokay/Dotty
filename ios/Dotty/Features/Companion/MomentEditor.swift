import SwiftUI

/// One reminder or alarm: when (once or repeating), what it's about, how it's said and by whom. Dotty
/// writes and records the line itself (in a chat, or when it next reaches this iPhone).
struct MomentEditor: View {
    @State var moment: DottyMoment
    let companions: [MomentsView.CompanionRef]
    let onSaved: () -> Void
    @Environment(DottyLink.self) private var link
    @Environment(\.dismiss) private var dismiss
    @State private var saving = false
    @State private var confirmDelete = false
    @State private var error: String?

    private static let styles = [("spoken", "Spoken"), ("sung", "Sung"), ("whispered", "Whispered"),
                                 ("excited", "Excited"), ("calm", "Calm")]
    private static let dayKeys = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]

    private var isNew: Bool { moment.momentId == 0 }
    private var canSave: Bool {
        !moment.what.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && (moment.repeat != "days" || !moment.days.isEmpty)
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.l) {
                    PageHeader(title: isNew ? "New moment" : moment.what, subtitle: nil)
                        .padding(.top, Spacing.l)
                    if let error { NoticeCard(kind: .error, text: error) }
                    GlassCard {
                        VStack(alignment: .leading, spacing: Spacing.m) {
                            Picker("Kind", selection: $moment.kind) {
                                Text("Reminder").tag("reminder")
                                Text("Alarm").tag("alarm")
                            }
                            .pickerStyle(.segmented)
                            Text(moment.kind == "alarm" ? "Rings every minute until you press BOOT (a tap snoozes 5 minutes)."
                                                         : "Said once; hold BOOT within 3 minutes to answer.")
                                .font(.lpCaption).foregroundStyle(Color.inkMuted)
                            if moment.repeat == "once" {  // a date and time still to come
                                DatePicker("When", selection: dateBinding, in: Date()..., displayedComponents: [.date, .hourAndMinute])
                                    .font(.lpBody)
                            } else {  // any time of day: an earlier one than now starts tomorrow
                                DatePicker("At", selection: dateBinding, displayedComponents: [.hourAndMinute])
                                    .font(.lpBody)
                            }
                            Picker("Repeat", selection: $moment.repeat) {
                                Text("Once").tag("once")
                                Text("Every day").tag("daily")
                                Text("Weekdays").tag("weekdays")
                                Text("Chosen days").tag("days")
                            }
                            .font(.lpBody)
                            if moment.repeat == "days" {
                                HStack {
                                    ForEach(0..<7, id: \.self) { i in
                                        let key = Self.dayKeys[i]
                                        let on = moment.days.contains(key)
                                        Button(String(key.prefix(2)).capitalized) {
                                            if on { moment.days.removeAll { $0 == key } } else { moment.days.append(key) }
                                        }
                                        .buttonStyle(.borderedProminent)
                                        .tint(on ? DottyLight.firefly.color : Color.gray.opacity(0.3))
                                        .font(.caption)
                                    }
                                }
                            }
                            VStack(alignment: .leading, spacing: 4) {
                                HStack {
                                    Text("What it's about").font(.lpLabel).foregroundStyle(Color.inkMuted)
                                    Spacer()
                                    Text("\(moment.what.count)/200").font(.lpCaption).foregroundStyle(Color.inkFaint)
                                }
                                TextField("Cook dinner", text: $moment.what, axis: .vertical)
                                    .lineLimit(1...4)
                                    .font(.lpBody).foregroundStyle(Color.ink)
                                    .padding(Spacing.s)
                                    .background(RoundedRectangle(cornerRadius: 10).fill(Color.glassStrong))
                                    .onChange(of: moment.what) { _, v in if v.count > 200 { moment.what = String(v.prefix(200)) } }
                            }
                            Picker("How it's said", selection: $moment.style) {
                                ForEach(Self.styles, id: \.0) { Text($0.1).tag($0.0) }
                            }
                            .font(.lpBody)
                            if companions.count > 1 {
                                Picker("Who says it", selection: $moment.companion) {
                                    ForEach(companions) { Text($0.name.isEmpty ? "Not awake yet" : $0.name).tag($0.id) }
                                }
                                .font(.lpBody)
                            }
                        }
                        .padding(Spacing.m)
                    }
                    HStack {
                        if !isNew {
                            Button("Delete", role: .destructive) { confirmDelete = true }.buttonStyle(.quiet())
                        }
                        Spacer()
                        if saving { ProgressView().controlSize(.small) }
                        Button("Save") { Task { await save() } }
                            .buttonStyle(.light())
                            .disabled(!canSave || saving)
                    }
                    .needsDotty(link)
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
        }
        .keyboardDismissing()
        .navigationTitle("")
        .confirmationDialog("Delete this \(moment.kind)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) { Task { await delete() } }
        }
    }

    private var dateBinding: Binding<Date> {
        Binding(get: { moment.date }, set: { moment.at = DottyTime.local($0) })
    }

    private func save() async {
        saving = true
        defer { saving = false }
        var at = moment.at
        if moment.repeat != "once" && at <= DottyTime.local(Date()) { at += 86400 }  // a repeating one starts tomorrow
        var args: [String: Any] = ["companion": moment.companion, "kind": moment.kind, "at": at, "repeat": moment.repeat,
                                   "what": moment.what.trimmingCharacters(in: .whitespacesAndNewlines), "style": moment.style]
        if moment.repeat == "days" { args["days"] = moment.days }
        if !isNew { args["id"] = moment.momentId }
        do {
            let r = try await link.send("companion.moment.save", args)
            if r.ok == false {
                error = Self.words(r["error"] as? String ?? "")
                return
            }
            onSaved()
            dismiss()
        } catch {
            if !link.lostConnection(error) { self.error = Self.words(error.localizedDescription) }
        }
    }

    private func delete() async {
        do {
            _ = try await link.send("companion.moment.delete", ["companion": moment.companion, "id": moment.momentId])
            onSaved()
            dismiss()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    static func words(_ error: String) -> String {
        switch error {
        case "past": return "That time has passed. Pick a later one."
        case "full": return "Dotty holds 50 moments for this companion. Delete one first."
        case "unavailable": return "Dotty needs its SD card for moments."
        case "no days": return "Pick at least one day."
        default: return error.isEmpty ? "That didn't work." : error
        }
    }
}
