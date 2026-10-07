import SwiftUI

/// The Pet cartridge's screen (firmware: cartridges/pet/): how the pet is doing, Pause, its
/// sounds and quiet hours, and the pets that came before. Looking after it happens on Dotty.
struct PetView: View {
    @Environment(DottyLink.self) private var link
    @State private var status: Status?
    @State private var history: [Memory] = []
    @State private var error: String?
    @State private var confirmNewEgg = false

    struct Status {
        var species = "", stage = "", needs = ""
        var generation = 1, age = 0, weight = 0, hunger = 0, happy = 0, discipline = 0
        var careMistakes = 0, disciplineMistakes = 0, poops = 0
        var sick = false, asleep = false, lightsOn = true, paused = false, calling = false
        var died: String?
        var soundOn = true, volume = 60.0, quietFrom = 1410, quietTo = 480
    }

    struct Memory: Identifiable {
        var id: Int { generation }
        let generation: Int
        let species: String
        let age: Int
        let cause: String
        let died: Date?
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Pet", subtitle: "Look after it on Dotty: feed it, play, clean up, turn off the light at bedtime. Rules of the 1996 original.")
                        .padding(.top, Spacing.l)
                    NotConnectedNotice()
                    if let error { NoticeCard(kind: .error, text: error) }
                    if let status {
                        if status.calling || status.sick || status.hunger == 0 || status.happy == 0 || status.poops > 0,
                           status.died == nil, status.stage != "egg", !status.paused {
                            NoticeCard(kind: .error, text: "\(status.needs) Unlock Dotty to help.")
                        }
                        statusCard(status)
                        pauseCard(status).needsDotty(link)
                        soundCard(status).needsDotty(link)
                    }
                    memoriesCard.needsDotty(link)
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
            if link.lastEvent?.event == "pet.changed" { Task { await load() } }
        }
        .confirmationDialog("Start over with a new egg?", isPresented: $confirmNewEgg, titleVisibility: .visible) {
            Button("New egg", role: .destructive) { Task { await newEgg(force: true) } }
        } message: {
            Text("The pet you have now is gone for good.")
        }
    }

    // MARK: - Cards

    private func statusCard(_ s: Status) -> some View {
        GlassCard(title: title(s)) {
            if s.stage != "egg", s.died == nil {
                LightRow(title: "Hunger", systemImage: "fork.knife") { hearts(s.hunger) }
                LightRow(title: "Happy", systemImage: "face.smiling") { hearts(s.happy) }
                LightRow(title: "Discipline", systemImage: "hand.raised") {
                    LightProgress(value: Double(s.discipline) / 100).frame(width: 110)
                }
                LightRow(title: "Age \(s.age) · \(s.weight) g", subtitle: details(s), systemImage: "calendar")
            } else if let died = s.died {
                LightRow(title: "Passed away at age \(s.age)", subtitle: "From \(died). Tap Dotty's screen for a new egg.",
                         systemImage: "sparkles")
            } else {
                LightRow(title: "An egg", subtitle: "It hatches 5 minutes after it's laid.", systemImage: "oval.portrait")
            }
        }
    }

    private func title(_ s: Status) -> String {
        if s.stage == "egg" { return "Generation \(s.generation)" }
        return "\(s.species) · \(s.stage) · generation \(s.generation)"
    }

    private func details(_ s: Status) -> String {
        var parts: [String] = []
        if s.asleep { parts.append(s.lightsOn ? "asleep, light on" : "asleep") }
        if s.sick { parts.append("sick") }
        if s.poops > 0 { parts.append("\(s.poops) to clean") }
        parts.append("\(s.careMistakes) care / \(s.disciplineMistakes) discipline mistakes")
        return parts.joined(separator: " · ")
    }

    private func hearts(_ n: Int) -> some View {
        HStack(spacing: 3) {
            ForEach(0..<4, id: \.self) { i in
                Image(systemName: i < n ? "heart.fill" : "heart")
                    .foregroundStyle(i < n ? DottyLight.ember.color : Color.inkFaint)
            }
        }
    }

    private func pauseCard(_ s: Status) -> some View {
        GlassCard(title: "Away for a while?") {
            Toggle(isOn: Binding(get: { s.paused }, set: { on in Task { await send("pet.pause", ["on": on]) } })) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Pause").font(.lpHeadline).foregroundStyle(Color.ink)
                    Text("Time stops for it: no hunger, no growing up. It also pauses whenever Dotty is off or runs another cartridge.")
                        .font(.lpCaption).foregroundStyle(Color.inkMuted)
                }
            }
            .toggleStyle(.light())
            .padding(Spacing.m)
        }
    }

    private func soundCard(_ s: Status) -> some View {
        GlassCard(title: "Sounds") {
            Toggle(isOn: Binding(get: { s.soundOn }, set: { on in Task { await send("pet.sound", ["on": on]) } })) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Chirps").font(.lpHeadline).foregroundStyle(Color.ink)
                    Text("One chirp when it calls, also on the lock screen. Never repeated.")
                        .font(.lpCaption).foregroundStyle(Color.inkMuted)
                }
            }
            .toggleStyle(.light())
            .padding(Spacing.m)
            HStack {
                Image(systemName: "speaker.wave.1").foregroundStyle(Color.inkMuted)
                Slider(value: Binding(get: { s.volume }, set: { status?.volume = $0 }), in: 10...100) { editing in
                    if !editing { Task { await send("pet.sound", ["volume": Int(status?.volume ?? 60)]) } }
                }
                Image(systemName: "speaker.wave.3").foregroundStyle(Color.inkMuted)
            }
            .padding(.horizontal, Spacing.m)
            HStack {
                Text("Quiet from").font(.lpCallout).foregroundStyle(Color.ink)
                Spacer()
                DatePicker("", selection: timeBinding(\.quietFrom, key: "quietFrom"), displayedComponents: .hourAndMinute)
                    .labelsHidden()
                Text("to").font(.lpCallout).foregroundStyle(Color.ink)
                DatePicker("", selection: timeBinding(\.quietTo, key: "quietTo"), displayedComponents: .hourAndMinute)
                    .labelsHidden()
            }
            .padding(Spacing.m)
        }
    }

    /// A minutes-of-the-day setting shown as a time picker.
    private func timeBinding(_ field: WritableKeyPath<Status, Int>, key: String) -> Binding<Date> {
        Binding(get: {
            Calendar.current.startOfDay(for: .now).addingTimeInterval(Double(status?[keyPath: field] ?? 0) * 60)
        }, set: { date in
            let parts = Calendar.current.dateComponents([.hour, .minute], from: date)
            let minutes = (parts.hour ?? 0) * 60 + (parts.minute ?? 0)
            status?[keyPath: field] = minutes
            Task { await send("pet.sound", [key: minutes]) }
        })
    }

    private var memoriesCard: some View {
        GlassCard(title: "Hall of memories") {
            if history.isEmpty {
                LightRow(title: "No pets before this one", systemImage: "heart.text.square")
            }
            ForEach(history) { m in
                LightRow(title: "\(m.species), age \(m.age)",
                         subtitle: "Generation \(m.generation) · \(m.cause)" + (m.died.map { " · \($0.formatted(date: .abbreviated, time: .omitted))" } ?? ""),
                         systemImage: "sparkles")
            }
            Button(status?.died != nil ? "New egg" : "Start over…") {
                if status?.died != nil { Task { await newEgg(force: false) } } else { confirmNewEgg = true }
            }
            .buttonStyle(.light())
            .padding(Spacing.m)
        }
    }

    // MARK: - Dotty

    private func load() async {
        do {
            let r = try await link.send("pet.status")
            var s = Status()
            s.species = r["species"] as? String ?? ""
            s.stage = r["stage"] as? String ?? ""
            s.needs = r["needs"] as? String ?? ""
            s.generation = r["generation"] as? Int ?? 1
            s.age = r["age"] as? Int ?? 0
            s.weight = r["weight"] as? Int ?? 0
            s.hunger = r["hunger"] as? Int ?? 0
            s.happy = r["happy"] as? Int ?? 0
            s.discipline = r["discipline"] as? Int ?? 0
            s.careMistakes = r["careMistakes"] as? Int ?? 0
            s.disciplineMistakes = r["disciplineMistakes"] as? Int ?? 0
            s.poops = r["poops"] as? Int ?? 0
            s.sick = r["sick"] as? Bool ?? false
            s.asleep = r["asleep"] as? Bool ?? false
            s.lightsOn = r["lightsOn"] as? Bool ?? true
            s.paused = r["paused"] as? Bool ?? false
            s.calling = r["calling"] as? Bool ?? false
            s.died = r["died"] as? String
            if let sound = r["sound"] as? [String: Any] {
                s.soundOn = sound["on"] as? Bool ?? true
                s.volume = Double(sound["volume"] as? Int ?? 60)
                s.quietFrom = sound["quietFrom"] as? Int ?? 1410
                s.quietTo = sound["quietTo"] as? Int ?? 480
            }
            status = s
            error = nil
            let h = try await link.send("pet.history")
            let offset = Double(TimeZone.current.secondsFromGMT())
            history = (h["pets"] as? [[String: Any]] ?? []).compactMap { item in
                guard let generation = item["generation"] as? Int else { return nil }
                // Dotty keeps local time: its minutes since 1970 are the local clock's reading.
                let died = (item["died"] as? Double).map { Date(timeIntervalSince1970: $0 * 60 - offset) }
                return Memory(generation: generation, species: item["species"] as? String ?? "",
                              age: item["age"] as? Int ?? 0, cause: item["cause"] as? String ?? "", died: died)
            }
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func send(_ command: String, _ args: [String: Any]) async {
        do {
            try await link.send(command, args)
            await load()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func newEgg(force: Bool) async {
        await send("pet.newEgg", ["force": force])
    }
}
