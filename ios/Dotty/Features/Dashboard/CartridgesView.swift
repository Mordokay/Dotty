import SwiftUI

/// The catalog from GitHub. Installing asks Dotty to fetch the cartridge over Wi-Fi onto its
/// SD card (skipped if it's already there) and boot it.
struct CartridgesView: View {
    @Environment(DottyLink.self) private var link
    @State private var catalog: Catalog?
    @State private var loadError: String?
    @State private var install: InstallState?
    @State private var confirmOlder: CatalogCartridge?
    @State private var success: String?
    /// What each cartridge has on Dotty's SD card (storage.list), by id.
    @State private var onCard: [String: CardFiles] = [:]
    @State private var confirmRemove: CatalogCartridge?
    @State private var removing: String?

    struct CardFiles {
        let versions: [String]
        let dataBytes: Int64
    }

    /// How a catalog entry relates to what's on Dotty.
    enum Relation {
        case notInstalled
        case current(running: Bool)
        case update(from: String)
        case older(installed: String)
    }

    struct InstallState {
        var cartridge: CatalogCartridge
        var stage = "Getting ready"
        var progress: Double?
        var error: String?
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Cartridges",
                               subtitle: "Dotty downloads them over Wi-Fi and keeps a copy on its SD card, so switching back is quick.")
                        .padding(.top, Spacing.l)
                    // During an install Dotty restarts on purpose; the install card explains.
                    if install == nil { NotConnectedNotice() }
                    if let install { installCard(install) }
                    if let loadError { NoticeCard(kind: .error, text: loadError) }
                    if let catalog {
                        // The cartridge being installed is shown by the progress card instead.
                        // Other cartridges are dimmed and inert until the install finishes.
                        let installing = install != nil && install?.error == nil
                        ForEach(catalog.cartridges.filter { $0.id != install?.cartridge.id }) { cartridge in
                            cartridgeCard(cartridge)
                                .opacity(installing ? 0.4 : 1)
                                .allowsHitTesting(!installing)
                                .animation(.settle, value: installing)
                        }
                    } else if loadError == nil {
                        HStack { Spacer(); FireflyLoader(size: 96, label: "Loading the catalog"); Spacer() }
                            .padding(.top, Spacing.xxl)
                    }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await load() }
        }
        .navigationTitle("")
        .overlay(alignment: .top) {
            if let success {
                NoticeCard(kind: .success, text: success)
                    .padding(.horizontal, Spacing.l)
                    .padding(.top, Spacing.s)
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
        }
        .animation(.settle, value: success)
        .sensoryFeedback(.success, trigger: success) { _, new in new != nil }
        .task { await load() }
        .task(id: link.info?.id) { await loadCard() }
        .confirmationDialog(confirmRemove.map { "Remove \($0.name) from Dotty?" } ?? "",
                            isPresented: Binding(get: { confirmRemove != nil }, set: { if !$0 { confirmRemove = nil } }),
                            titleVisibility: .visible, presenting: confirmRemove) { cartridge in
            let data = onCard[cartridge.id]?.dataBytes ?? 0
            if data > 0 {
                Button("Remove, keep \(dataName(cartridge)) (\(bytes(data)))") { Task { await remove(cartridge, withData: false) } }
                Button("Remove everything", role: .destructive) { Task { await remove(cartridge, withData: true) } }
            } else {
                Button("Remove", role: .destructive) { Task { await remove(cartridge, withData: true) } }
            }
        } message: { cartridge in
            Text((onCard[cartridge.id]?.dataBytes ?? 0) > 0
                 ? "Its firmware is deleted from the SD card. Keep \(dataName(cartridge)) to have them back if you install it again."
                 : "Its firmware is deleted from the SD card.")
        }
        .confirmationDialog("Install an older version?", isPresented: Binding(
            get: { confirmOlder != nil }, set: { if !$0 { confirmOlder = nil } }), titleVisibility: .visible,
            presenting: confirmOlder) { cartridge in
            Button("Install \(cartridge.version)", role: .destructive) { Task { await installCartridge(cartridge) } }
        } message: { cartridge in
            Text("Dotty has a newer \(cartridge.name). Installing \(cartridge.version) replaces it.")
        }
        .onChange(of: link.lastEvent?.raw) { _, _ in
            guard let event = link.lastEvent, event.event == "fetch.progress", install != nil else { return }
            let stage = event["stage"] as? String
            install?.stage = stage == "install" ? "Installing from the card" : stage == "catalog" ? "Reading the catalog" : "Downloading"
            if let done = event["done"] as? Double, let size = event["size"] as? Double, size > 0 {
                install?.progress = done / size
            }
        }
    }

    private func cartridgeCard(_ cartridge: CatalogCartridge) -> some View {
        let relation = relation(to: cartridge)
        return GlassCard {
            HStack(alignment: .top, spacing: Spacing.l) {
                CartridgeArtwork(cartridge: cartridge)

                VStack(alignment: .leading, spacing: Spacing.xs) {
                    Text(cartridge.name).font(.lpTitle).foregroundStyle(Color.ink)
                    Text("Version \(cartridge.version) · \(cartridge.sizeText)")
                        .font(.lpCaption).foregroundStyle(Color.inkMuted)
                    if !cartridge.description.isEmpty {
                        Text(cartridge.description)
                            .font(.lpCallout).foregroundStyle(Color.inkMuted)
                            .fixedSize(horizontal: false, vertical: true)
                            .padding(.top, Spacing.xs)
                    }
                    actions(for: cartridge, relation: relation)
                        .padding(.top, Spacing.s)
                        .needsDotty(link)
                    removeRow(for: cartridge, relation: relation)
                        .needsDotty(link)
                }
            }
            .padding(Spacing.m)
        }
    }

    @ViewBuilder
    private func actions(for cartridge: CatalogCartridge, relation: Relation) -> some View {
        let busy = install != nil && install?.error == nil
        switch relation {
        case .current(let running):
            HStack(spacing: Spacing.m) {
                StatusPill(state: .connected, text: running ? "Running" : "Installed")
                if running, let route = DashboardView.Route.screen(for: link.info) {
                    NavigationLink("Open", value: route).buttonStyle(.light())
                }
            }
        case .update(let from):
            VStack(alignment: .leading, spacing: Spacing.xs) {
                Button("Update to \(cartridge.version)") { Task { await installCartridge(cartridge) } }
                    .buttonStyle(.light(DottyLight.leaf.color))
                    .disabled(busy)
                Text("You have \(from)").font(.lpCaption).foregroundStyle(Color.inkMuted)
            }
        case .older(let installed):
            VStack(alignment: .leading, spacing: Spacing.xs) {
                Button("Install older \(cartridge.version)") { confirmOlder = cartridge }
                    .buttonStyle(.quiet())
                    .disabled(busy)
                Text("Dotty has a newer \(installed)").font(.lpCaption).foregroundStyle(Color.inkMuted)
            }
        case .notInstalled:
            Button("Install") { Task { await installCartridge(cartridge) } }
                .buttonStyle(.light())
                .disabled(busy)
        }
    }

    /// "On Dotty: 2 versions · songs and playlists 85 MB" and Remove, for cartridges on Dotty.
    @ViewBuilder
    private func removeRow(for cartridge: CatalogCartridge, relation: Relation) -> some View {
        let files = onCard[cartridge.id]
        let installed: Bool = { if case .notInstalled = relation { return false } else { return true } }()
        if (installed || files != nil), link.info?.supports("storage") == true {
            HStack(spacing: Spacing.m) {
                Text(cardSummary(cartridge, files: files, installed: installed))
                    .font(.lpCaption).foregroundStyle(Color.inkMuted)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 0)
                Button(removing == cartridge.id ? "Removing…" : "Remove") { confirmRemove = cartridge }
                    .buttonStyle(.quiet(DottyLight.ember.color))
                    .disabled(removing != nil || (install != nil && install?.error == nil))
            }
            .padding(.top, Spacing.xs)
        }
    }

    private func cardSummary(_ cartridge: CatalogCartridge, files: CardFiles?, installed: Bool) -> String {
        var parts: [String] = []
        if installed { parts.append("Installed") }
        if let files, !files.versions.isEmpty {
            parts.append(files.versions.count == 1 ? "1 copy on the card" : "\(files.versions.count) copies on the card")
        }
        if let files, files.dataBytes > 0 { parts.append("\(dataName(cartridge)) \(bytes(files.dataBytes))") }
        return parts.joined(separator: " · ")
    }

    private func dataName(_ cartridge: CatalogCartridge) -> String {
        cartridge.id == "music" ? "songs and playlists" : "its data"
    }

    private func bytes(_ count: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: count, countStyle: .file)
    }

    /// Compares with the running cartridge, or the installed one while the launcher is up.
    private func relation(to cartridge: CatalogCartridge) -> Relation {
        guard let info = link.info else { return .notInstalled }
        let current = info.isLauncher ? info.installed.map { ($0.id, $0.version) } : (info.id, info.version)
        guard let (id, version) = current, id == cartridge.id else { return .notInstalled }
        switch cartridge.version.compare(version, options: .numeric) {
        case .orderedSame: return .current(running: !info.isLauncher)
        case .orderedDescending: return .update(from: version)
        case .orderedAscending: return .older(installed: version)
        }
    }

    private func installCard(_ state: InstallState) -> some View {
        GlassCard(title: state.cartridge.name, light: DottyLight.firefly.color) {
            VStack(alignment: .leading, spacing: Spacing.m) {
                if let error = state.error {
                    Text(error).font(.lpCallout).foregroundStyle(DottyLight.ember.color)
                    Button("Close") { install = nil }.buttonStyle(.quiet())
                } else {
                    Text(state.stage).font(.lpCallout).foregroundStyle(Color.ink)
                    if let progress = state.progress {
                        LightProgress(value: progress)
                    } else {
                        HStack { Spacer(); FireflyLoader(size: 64, label: state.stage); Spacer() }
                    }
                    Text("Keep Dotty awake and nearby.").font(.lpCaption).foregroundStyle(Color.inkMuted)
                }
            }
            .padding(Spacing.m)
        }
    }

    private func load() async {
        do {
            catalog = try await Catalog.load()
            loadError = nil
        } catch {
            loadError = "Couldn't load the catalog. Check your internet connection."
        }
    }

    private func loadCard() async {
        guard link.connection == .connected, link.info?.supports("storage") == true,
              let reply = try? await link.send("storage.list") else { return }
        var found: [String: CardFiles] = [:]
        for item in reply["cartridges"] as? [[String: Any]] ?? [] {
            guard let id = item["id"] as? String else { continue }
            found[id] = CardFiles(versions: item["versions"] as? [String] ?? [],
                                  dataBytes: (item["data"] as? NSNumber)?.int64Value ?? 0)
        }
        onCard = found
    }

    /// Deletes the cartridge's firmware copies (and, with withData, its files and settings).
    /// A running cartridge can't remove itself, so Dotty switches to the launcher first; the
    /// launcher also empties the slot when the installed cartridge goes.
    private func remove(_ cartridge: CatalogCartridge, withData: Bool) async {
        removing = cartridge.id
        defer { removing = nil }
        do {
            if let info = link.info, !info.isLauncher, info.id == cartridge.id {
                try await link.ensureLauncher()
            }
            try await link.send("storage.remove", ["id": cartridge.id, "data": withData])
            link.refreshInfo()
            await loadCard()
            success = withData ? "\(cartridge.name) was removed."
                               : "\(cartridge.name) was removed. Its \(dataName(cartridge)) stay on the card."
            try? await Task.sleep(for: .seconds(2.5))
            success = nil
        } catch {
            loadError = "Couldn't remove \(cartridge.name): \(error.localizedDescription)"
        }
    }

    private func installCartridge(_ cartridge: CatalogCartridge) async {
        install = InstallState(cartridge: cartridge)
        do {
            // The launcher does the install and always has the Wi-Fi commands (older
            // cartridges may not), so switch first, then check Wi-Fi.
            install?.stage = "Switching to the launcher"
            try await link.ensureLauncher()
            install?.stage = "Checking Dotty's Wi-Fi"
            let saved = try await link.send("wifi.list")["networks"] as? [String] ?? []
            guard !saved.isEmpty else {
                install?.error = "Dotty has no Wi-Fi yet. Add a network under Wi-Fi first."
                return
            }
            install?.stage = "Connecting to Wi-Fi"
            // Name/version/size let Dotty's screen show the cartridge before the catalog arrives.
            try await link.send("library.fetch", ["id": cartridge.id, "name": cartridge.name, "version": cartridge.version,
                                                  "size": cartridge.size, "sha256": cartridge.sha256], timeout: 300)
            install?.stage = "Restarting"
            install?.progress = nil
            try await link.waitForReconnect()
            // Short confirmation that goes away by itself.
            install = nil
            await loadCard()
            success = "\(cartridge.name) \(cartridge.version) is installed."
            try? await Task.sleep(for: .seconds(2.5))
            success = nil
        } catch {
            install?.error = error.localizedDescription
        }
    }
}
