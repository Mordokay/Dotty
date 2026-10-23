import SwiftUI

/// Macros (firmware: cartridges/companion/macro_store.*): say one of a macro's phrases to any
/// companion and Dotty has its prompt written (~150 words, in that companion's voice) and read aloud.
/// Shared by every companion; changes apply from the next chat.
struct MacrosView: View {
    @Environment(DottyLink.self) private var link
    @State private var macros: [Macro] = []
    @State private var max = 12
    @State private var loaded = false
    @State private var error: String?

    struct Macro: Identifiable, Hashable {
        var id: Int
        var name: String
        var triggers: String
        var prompt: String
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Macros",
                               subtitle: "Say one of a macro's phrases and the companion on Dotty writes it and reads it to you.")
                        .padding(.top, Spacing.l)
                    NotConnectedNotice()
                    if let error { NoticeCard(kind: .error, text: error) }
                    GlassCard(title: "Your macros") {
                        if loaded && macros.isEmpty {
                            LightRow(title: "No macros yet", subtitle: "Add one below", systemImage: "wand.and.stars")
                        }
                        ForEach(macros) { m in
                            NavigationLink {
                                MacroEditor(macro: m, onSaved: { Task { await load() } })
                            } label: {
                                LightRow(title: m.name,
                                         subtitle: m.triggers.split(separator: "\n").map { "\u{201C}\($0)\u{201D}" }.joined(separator: " · "),
                                         systemImage: "wand.and.stars") {
                                    Image(systemName: "chevron.right").font(.system(size: 14, weight: .semibold, design: .rounded))
                                }
                            }
                            .buttonStyle(.plain)
                        }
                        if macros.count < max {
                            NavigationLink {
                                MacroEditor(macro: Macro(id: 0, name: "", triggers: "", prompt: ""), onSaved: { Task { await load() } })
                            } label: {
                                LightRow(title: "New macro", systemImage: "plus") {
                                    Image(systemName: "chevron.right").font(.system(size: 14, weight: .semibold, design: .rounded))
                                }
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    .needsDotty(link)
                    Text("Changes apply from the next chat on Dotty. A reading lasts about a minute (~150 words).")
                        .font(.lpCaption).foregroundStyle(Color.inkFaint)
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await load() }
        }
        .navigationTitle("")
        .task { await load() }
        .onChange(of: link.eventCount) { _, _ in
            if link.lastEvent?.event == "companion.macros" { Task { await load() } }
        }
    }

    private func load() async {
        do {
            let r = try await link.send("companion.macros")
            macros = (r["macros"] as? [[String: Any]] ?? []).map {
                Macro(id: $0["id"] as? Int ?? 0, name: $0["name"] as? String ?? "",
                      triggers: $0["triggers"] as? String ?? "", prompt: $0["prompt"] as? String ?? "")
            }
            max = r["max"] as? Int ?? 12
            loaded = true
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}

/// One macro: its name, the phrases that start it, and the prompt (with ✨ Suggest).
struct MacroEditor: View {
    @State var macro: MacrosView.Macro
    let onSaved: () -> Void
    @Environment(DottyLink.self) private var link
    @Environment(\.dismiss) private var dismiss
    @State private var saving = false
    @State private var suggesting = false
    @State private var confirmDelete = false
    @State private var error: String?

    private var isNew: Bool { macro.id == 0 }
    private var canSave: Bool {
        !macro.triggers.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
            !macro.prompt.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.l) {
                    PageHeader(title: isNew ? "New macro" : macro.name, subtitle: nil)
                        .padding(.top, Spacing.l)
                    if let error { NoticeCard(kind: .error, text: error) }
                    GlassCard {
                        VStack(alignment: .leading, spacing: Spacing.m) {
                            field("Name") {
                                TextField("Talk dirty to me", text: $macro.name)
                                    .onChange(of: macro.name) { _, v in if v.count > 40 { macro.name = String(v.prefix(40)) } }
                            }
                            field("Say any of these (one per line)") {
                                TextField("talk dirty to me\nflirt with me", text: $macro.triggers, axis: .vertical)
                                    .lineLimit(2...6)
                                    .onChange(of: macro.triggers) { _, v in if v.count > 300 { macro.triggers = String(v.prefix(300)) } }
                            }
                            VStack(alignment: .leading, spacing: 4) {
                                HStack {
                                    Text("What to write").font(.lpLabel).foregroundStyle(Color.inkMuted)
                                    Spacer()
                                    if suggesting {
                                        ProgressView().controlSize(.mini)
                                    } else {
                                        Button { Task { await suggest() } } label: {
                                            Label("Suggest", systemImage: "sparkles").font(.lpCaption)
                                        }
                                        .buttonStyle(.plain)
                                        .foregroundStyle(DottyLight.firefly.color)
                                    }
                                    Text("\(macro.prompt.count)/2000").font(.lpCaption).foregroundStyle(Color.inkFaint)
                                }
                                TextField("What should the companion say? The tone, what to include, how to end.",
                                          text: $macro.prompt, axis: .vertical)
                                    .lineLimit(8...30)
                                    .font(.lpBody).foregroundStyle(Color.ink)
                                    .padding(Spacing.s)
                                    .background(RoundedRectangle(cornerRadius: 10).fill(Color.glassStrong))
                                    .onChange(of: macro.prompt) { _, v in if v.count > 2000 { macro.prompt = String(v.prefix(2000)) } }
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
        .confirmationDialog("Delete \(macro.name)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) { Task { await delete() } }
        }
    }

    private func field<Content: View>(_ title: String, @ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.lpLabel).foregroundStyle(Color.inkMuted)
            content()
                .font(.lpBody).foregroundStyle(Color.ink)
                .padding(Spacing.s)
                .background(RoundedRectangle(cornerRadius: 10).fill(Color.glassStrong))
        }
    }

    /// ✨ Gemini rewrites the prompt: the macro's name and phrases, and what's written so far.
    private func suggest() async {
        suggesting = true
        defer { suggesting = false }
        let (prompt, schema) = CompanionDesign.macro(name: macro.name, triggers: macro.triggers, current: macro.prompt)
        do {
            let answer = try await GeminiText.generate(prompt, schema: schema)
            if let value = answer["value"] as? String, !value.isEmpty { macro.prompt = String(value.prefix(2000)) }
            error = nil
        } catch {
            self.error = error.localizedDescription
        }
    }

    /// Name and phrases in one command; the prompt inline when short, else in pieces (commands ≤ 512
    /// bytes, JSON escapes included: 240 bytes a piece, as the character fields).
    private func save() async {
        saving = true
        defer { saving = false }
        do {
            let short = macro.prompt.utf8.count <= 240
            var args: [String: Any] = ["name": macro.name, "triggers": macro.triggers]
            if macro.id != 0 { args["id"] = macro.id }
            args["prompt"] = short ? macro.prompt : "..."  // a placeholder until the pieces arrive
            if !short && macro.id != 0 { args.removeValue(forKey: "prompt") }  // keep the old one meanwhile
            let r = try await link.send("companion.macro.save", args)
            let id = r["id"] as? Int ?? macro.id
            if !short {
                var pieces: [String] = [], current = ""
                for ch in macro.prompt {
                    if (current + String(ch)).utf8.count > 240 {
                        pieces.append(current)
                        current = ""
                    }
                    current.append(ch)
                }
                pieces.append(current)
                var offset = 0
                let total = macro.prompt.utf8.count
                for piece in pieces {
                    try await link.send("companion.macro.prompt", ["id": id, "offset": offset, "total": total, "text": piece])
                    offset += piece.utf8.count
                }
            }
            onSaved()
            dismiss()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func delete() async {
        do {
            try await link.send("companion.macro.delete", ["id": macro.id])
            onSaved()
            dismiss()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}
