import SwiftUI

/// Backups of Dotty's SD card on this iPhone: make one, put one back, or delete it.
struct BackupView: View {
    @Environment(DottyLink.self) private var link
    @State private var model: BackupModel?

    var body: some View {
        Group {
            if let model {
                BackupContent(model: model)
            } else {
                LightField { Color.clear }
            }
        }
        .onAppear { if model == nil { model = BackupModel(link: link) } }
    }
}

private struct BackupContent: View {
    @Bindable var model: BackupModel
    @Environment(DottyLink.self) private var link
    @State private var withFirmware = false
    /// Cartridges left out of the next backup (all are in by default).
    @State private var skipped: Set<String> = []
    @State private var restoring: BackupModel.Backup?
    @State private var deleting: BackupModel.Backup?

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Backups",
                               subtitle: "Copies of everything on Dotty's SD card, kept on this iPhone. You'll also find them in the Files app.")
                        .padding(.top, Spacing.l)
                    // Bluetooth pauses on purpose while files go over Wi-Fi.
                    if !model.busy { NotConnectedNotice() }
                    if let notice = model.notice { NoticeCard(kind: .success, text: notice) }
                    if let error = model.error { NoticeCard(kind: .error, text: error) }
                    if model.busy { progressCard } else { backUpCard }
                    backupsCard
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
        }
        .navigationTitle("")
        .onAppear { model.loadBackups() }
        .task(id: link.connection == .connected) {
            if link.connection == .connected && !model.busy { await model.loadOnDotty() }
        }
        .confirmationDialog("Put this backup back on Dotty?", isPresented: .init(get: { restoring != nil },
                                                                               set: { if !$0 { restoring = nil } }),
                            titleVisibility: .visible, presenting: restoring) { backup in
            Button("Restore") { model.start { await model.restore(backup) } }
        } message: { backup in
            Text("\(backup.cartridges.map { BackupModel.names[$0] ?? $0 }.joined(separator: ", ")) on Dotty will match this backup. Files that are already the same aren't sent again; other cartridges aren't touched.")
        }
        .confirmationDialog("Delete this backup?", isPresented: .init(get: { deleting != nil },
                                                                       set: { if !$0 { deleting = nil } }),
                            titleVisibility: .visible, presenting: deleting) { backup in
            Button("Delete", role: .destructive) { model.delete(backup) }
        } message: { _ in
            Text("It's removed from this iPhone. Dotty isn't touched.")
        }
    }

    private var backUpCard: some View {
        GlassCard(title: "Back up now") {
            if model.onDotty.isEmpty {
                Text(link.connection == .connected ? "Looking at Dotty's card…" : "Connect to Dotty to choose what to back up.")
                    .font(.lpCallout).foregroundStyle(Color.inkMuted).padding(Spacing.m)
            }
            ForEach(model.onDotty) { cartridge in
                Toggle(isOn: .init(get: { !skipped.contains(cartridge.id) },
                                   set: { if $0 { skipped.remove(cartridge.id) } else { skipped.insert(cartridge.id) } })) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(cartridge.name).font(.lpCallout).foregroundStyle(Color.ink)
                        Text(cartridge.bytes > 0 ? ByteCountFormatter.string(fromByteCount: cartridge.bytes, countStyle: .file) : "No files yet")
                            .font(.lpCaption).foregroundStyle(Color.inkMuted)
                    }
                }
                .toggleStyle(.light())
                .padding(.horizontal, Spacing.m)
                .padding(.vertical, Spacing.s)
            }
            Divider().padding(.horizontal, Spacing.m)
            Toggle("Include cartridge copies", isOn: $withFirmware)
                .toggleStyle(.light())
                .padding(Spacing.m)
            Text(withFirmware ? "The chosen cartridges' files and their firmware." : "Your songs, photos, recordings and settings files. Cartridges can always be downloaded again.")
                .font(.lpCaption).foregroundStyle(Color.inkMuted)
                .padding(.horizontal, Spacing.m)
            Button(chosen.count == model.onDotty.count ? "Back up Dotty" : "Back up \(chosen.count) of \(model.onDotty.count)") {
                let only = chosen
                model.start { await model.backUp(only: only, withFirmware: withFirmware) }
            }
            .buttonStyle(.light())
            .padding(Spacing.m)
            .disabled(chosen.isEmpty)
            .needsDotty(link)
        }
    }

    private var chosen: Set<String> { Set(model.onDotty.map(\.id)).subtracting(skipped) }

    private var progressCard: some View {
        GlassCard(title: "Working on it", light: DottyLight.firefly.color) {
            VStack(alignment: .leading, spacing: Spacing.s) {
                Text(model.stage ?? "").font(.lpCallout).foregroundStyle(Color.ink)
                if let progress = model.progress {
                    LightProgress(value: progress)
                } else {
                    HStack { Spacer(); FireflyLoader(size: 64, label: model.stage ?? ""); Spacer() }
                }
                if let detail = model.detail {
                    Text(detail).font(.lpCaption.monospacedDigit()).foregroundStyle(Color.inkMuted)
                }
                Text("Keep the app open; your screen stays on. Dotty's Bluetooth pauses meanwhile.")
                    .font(.lpCaption).foregroundStyle(Color.inkFaint)
                Button("Cancel", role: .cancel) { model.cancel() }
                    .buttonStyle(.light())
                    .disabled(model.cancelling)
                    .padding(.top, Spacing.s)
            }
            .padding(Spacing.m)
        }
    }

    private var backupsCard: some View {
        GlassCard(title: "On this iPhone") {
            if model.backups.isEmpty {
                Text("No backups yet.").font(.lpCallout).foregroundStyle(Color.inkMuted).padding(Spacing.m)
            }
            ForEach(model.backups) { backup in
                LightRow(title: backup.made.formatted(date: .abbreviated, time: .shortened), subtitle: backup.summary,
                         systemImage: "externaldrive") {
                    Menu {
                        Button("Restore to Dotty", systemImage: "arrow.uturn.backward") { restoring = backup }
                            .disabled(link.connection != .connected || model.busy)
                        Button("Delete", systemImage: "trash", role: .destructive) { deleting = backup }
                    } label: {
                        Image(systemName: "ellipsis.circle").font(.system(size: 20, weight: .semibold))
                            .frame(width: 36, height: 36)
                            .contentShape(Rectangle())
                    }
                    .foregroundStyle(Color.inkMuted)
                }
            }
        }
    }
}
