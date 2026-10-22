import SwiftUI

/// What the companion remembers about you (firmware: cartridges/companion/memory_store.*): by
/// group, searchable; add, edit, move, delete. Dotty saves most of them by itself while you talk;
/// changes made here apply from the next chat.
struct MemoriesView: View {
    @Environment(DottyLink.self) private var link
    @State private var memories: [Memory] = []
    @State private var total = 0
    @State private var max = 300
    @State private var loaded = false
    @State private var search = ""
    @State private var error: String?
    @State private var editing: Memory?
    @State private var adding = false
    @State private var confirmClear = false

    struct Memory: Identifiable, Hashable {
        let id: Int
        var group: String
        var text: String
        let by: String
        let changed: Date
    }

    /// The groups, in the firmware's order (memory_store.cpp).
    static let groups: [(id: String, label: String, symbol: String)] = [
        ("you", "About you", "person"), ("people", "People", "person.2"), ("likes", "Likes & dislikes", "heart"),
        ("plans", "Plans & dates", "calendar"), ("other", "Other", "sparkles"),
    ]

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Memories",
                               subtitle: "What your companion remembers about you. It saves most of these by itself while you talk.")
                        .padding(.top, Spacing.l)
                    NotConnectedNotice()
                    if let error { NoticeCard(kind: .error, text: error) }
                    TextField("Search", text: $search)
                        .font(.lpBody).foregroundStyle(Color.ink)
                        .padding(Spacing.m)
                        .background(RoundedRectangle(cornerRadius: 14).fill(Color.glassStrong))
                    if loaded && memories.isEmpty {
                        GlassCard(title: "Nothing yet") {
                            LightRow(title: "Tell Dotty about yourself", subtitle: "Or add something below",
                                     systemImage: "bookmark")
                        }
                    }
                    ForEach(Self.groups, id: \.id) { group in
                        let items = shown.filter { $0.group == group.id }
                        if !items.isEmpty {
                            GlassCard(title: group.label) {
                                ForEach(items) { memory in row(memory) }
                            }
                        }
                    }
                    GlassCard(title: "\(total) of \(max)") {
                        LightRow(title: "Add a memory", systemImage: "plus", action: { adding = true })
                            .disabled(total >= max)
                        if !memories.isEmpty {
                            LightRow(title: "Forget everything", systemImage: "trash", action: { confirmClear = true })
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
        .sheet(item: $editing) { memory in
            MemoryEditor(memory: memory, onSave: { text, group in
                Task { await send("companion.memory.update", ["id": memory.id, "text": text, "group": group]) }
            }, onDelete: {
                Task { await send("companion.memory.delete", ["id": memory.id]) }
            })
        }
        .sheet(isPresented: $adding) {
            MemoryEditor(memory: nil, onSave: { text, group in
                Task { await send("companion.memory.add", ["text": text, "group": group]) }
            }, onDelete: nil)
        }
        .confirmationDialog("Forget every memory?", isPresented: $confirmClear, titleVisibility: .visible) {
            Button("Forget everything", role: .destructive) { Task { await send("companion.memory.clear") } }
        } message: {
            Text("Dotty deletes everything it remembers about you.")
        }
    }

    private var shown: [Memory] {
        let q = search.trimmingCharacters(in: .whitespaces)
        return q.isEmpty ? memories : memories.filter { $0.text.localizedCaseInsensitiveContains(q) }
    }

    private func row(_ m: Memory) -> some View {
        Button { editing = m } label: {
            VStack(alignment: .leading, spacing: 2) {
                Text(m.text).font(.lpBody).foregroundStyle(Color.ink)
                    .fixedSize(horizontal: false, vertical: true)
                Text("\(m.by == "app" ? "by you" : "by Dotty") · \(m.changed.formatted(date: .abbreviated, time: .omitted))")
                    .font(.lpCaption).foregroundStyle(Color.inkMuted)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, Spacing.m)
            .padding(.vertical, Spacing.s)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .swipeActions {
            Button("Delete", role: .destructive) { Task { await send("companion.memory.delete", ["id": m.id]) } }
        }
        .contextMenu {
            Button("Delete", systemImage: "trash", role: .destructive) {
                Task { await send("companion.memory.delete", ["id": m.id]) }
            }
        }
        .needsDotty(link)
    }

    // MARK: - Dotty

    /// Every page (the list can be ~75 KB: 50 at a time).
    private func load() async {
        do {
            var all: [Memory] = []
            var offset = 0
            while true {
                let r = try await link.send("companion.memories", ["offset": offset, "limit": 50])
                if r["ok"] as? Bool == false {
                    error = (r["error"] as? String) == "unavailable" ? "Memories unavailable: Dotty can't read its SD card." : "Dotty couldn't list its memories."
                    return
                }
                let page = (r["memories"] as? [[String: Any]] ?? []).map { m in
                    Memory(id: m["id"] as? Int ?? 0, group: m["group"] as? String ?? "other", text: m["text"] as? String ?? "",
                           by: m["by"] as? String ?? "dotty",
                           changed: Date(timeIntervalSince1970: TimeInterval(m["changed"] as? Int ?? 0)))
                }
                all += page
                total = r["total"] as? Int ?? all.count
                max = r["max"] as? Int ?? 300
                offset += page.count
                if page.isEmpty || offset >= total { break }
            }
            memories = all
            loaded = true
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }

    private func send(_ cmd: String, _ args: [String: Any] = [:]) async {
        do {
            let r = try await link.send(cmd, args)
            if r["ok"] as? Bool == false {
                switch r["error"] as? String {
                case "full": error = "Memory full: delete some first."
                case "unavailable": error = "Memories unavailable: Dotty can't read its SD card."
                case "empty": error = "A memory needs some words."
                default: error = "Dotty couldn't do that."
                }
            } else {
                error = nil
            }
            await load()
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}

/// Add or edit one memory: up to 200 characters and a group.
private struct MemoryEditor: View {
    let memory: MemoriesView.Memory?
    let onSave: (String, String) -> Void
    let onDelete: (() -> Void)?
    @Environment(\.dismiss) private var dismiss
    @State private var text = ""
    @State private var group = "you"
    @State private var confirmDelete = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Your sister is called Ana", text: $text, axis: .vertical)
                        .lineLimit(3...6)
                        .onChange(of: text) { _, value in
                            if value.count > 200 { text = String(value.prefix(200)) }
                        }
                } footer: {
                    Text("\(text.count)/200 · Write it to you, the way Dotty would: \"You love swimming\".")
                }
                Section("Group") {
                    Picker("Group", selection: $group) {
                        ForEach(MemoriesView.groups, id: \.id) { g in
                            Label(g.label, systemImage: g.symbol).tag(g.id)
                        }
                    }
                    .pickerStyle(.inline)
                    .labelsHidden()
                }
                if let onDelete {
                    Section {
                        Button("Delete this memory", role: .destructive) { confirmDelete = true }
                            .confirmationDialog("Delete this memory?", isPresented: $confirmDelete) {
                                Button("Delete", role: .destructive) {
                                    onDelete()
                                    dismiss()
                                }
                            }
                    }
                }
            }
            .navigationTitle(memory == nil ? "New memory" : "Memory")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        onSave(text.trimmingCharacters(in: .whitespacesAndNewlines), group)
                        dismiss()
                    }
                    .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
            }
        }
        .onAppear {
            text = memory?.text ?? ""
            group = memory?.group ?? "you"
        }
    }
}
