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
                        if let launcher = catalog.launcher, install?.cartridge.id != launcher.id {
                            systemCard(launcher)
                                .opacity(installing ? 0.4 : 1)
                                .allowsHitTesting(!installing)
                        }
                        ForEach(ordered(catalog.cartridgeList).filter { $0.id != install?.cartridge.id }) { cartridge in
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

    /// The running cartridge first, then the installed one, then the rest in catalog order.
    private func ordered(_ cartridges: [CatalogCartridge]) -> [CatalogCartridge] {
        func rank(_ c: CatalogCartridge) -> Int {
            switch relation(to: c) {
            case .current(let running): running ? 0 : 1
            case .update, .older: 1
            case .notInstalled: 2
            }
        }
        return cartridges.enumerated()
            .sorted { (rank($0.element), $0.offset) < (rank($1.element), $1.offset) }
            .map(\.element)
    }

    /// Artwork, name over status, and one action on the right; the description below at full
    /// width. The running cartridge's panel glows.
    private func cartridgeCard(_ cartridge: CatalogCartridge) -> some View {
        let relation = relation(to: cartridge)
        let running: Bool = { if case .current(true) = relation { return true } else { return false } }()
        return VStack(alignment: .leading, spacing: Spacing.m) {
            HStack(alignment: .center, spacing: Spacing.m) {
                CartridgeArtwork(cartridge: cartridge, size: 56)
                VStack(alignment: .leading, spacing: 2) {
                    Text(cartridge.name)
                        .font(.lpTitle)
                        .foregroundStyle(Color.ink)
                        .lineLimit(1)
                        .minimumScaleFactor(0.75)
                    Text(statusLine(cartridge, relation))
                        .font(.lpCaption)
                        .foregroundStyle(running ? DottyLight.firefly.color : Color.inkMuted)
                        .lineLimit(1)
                }
                Spacer(minLength: Spacing.s)
                action(for: cartridge, relation: relation)
                    .needsDotty(link)
            }
            if !cartridge.description.isEmpty {
                Text(cartridge.description)
                    .font(.lpCallout)
                    .foregroundStyle(Color.inkMuted)
                    .fixedSize(horizontal: false, vertical: true)
            }
            removeRow(for: cartridge, relation: relation)
                .needsDotty(link)
        }
        .padding(Spacing.l)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background {
            if running {
                RoundedRectangle(cornerRadius: Radius.card, style: .continuous)
                    .fill(DottyLight.firefly.color.opacity(0.12))
            }
        }
        .glassSurface(light: running ? DottyLight.firefly.color : nil)
    }

    /// Under the name: version and size, or how it relates to what Dotty has.
    private func statusLine(_ cartridge: CatalogCartridge, _ relation: Relation) -> String {
        switch relation {
        case .current(let running): "\(running ? "Running" : "Installed") · v\(cartridge.version)"
        case .update(let from): "Update · \(from) → \(cartridge.version)"
        case .older(let installed): "v\(cartridge.version) · Dotty has \(installed)"
        case .notInstalled: "v\(cartridge.version) · \(cartridge.sizeText)"
        }
    }

    /// The one button on the right of the panel.
    @ViewBuilder
    private func action(for cartridge: CatalogCartridge, relation: Relation) -> some View {
        let busy = install != nil && install?.error == nil
        switch relation {
        case .current(let running):
            if running, let route = DashboardView.Route.screen(for: link.info) {
                NavigationLink("Open", value: route).buttonStyle(.light())
            }
        case .update:
            Button("Update") { Task { await installCartridge(cartridge) } }
                .buttonStyle(.light(DottyLight.leaf.color))
                .disabled(busy)
        case .older:
            Button("Install") { confirmOlder = cartridge }
                .buttonStyle(.quiet())
                .disabled(busy)
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

    // MARK: - Dotty system (the launcher)

    /// Dotty's system firmware: its version, and Update when the catalog has a newer one.
    private func systemCard(_ launcher: CatalogCartridge) -> some View {
        let current = link.info?.launcherVersion
        let newer = current.map { launcher.version.compare($0, options: .numeric) == .orderedDescending } ?? false
        return VStack(alignment: .leading, spacing: Spacing.m) {
            HStack(alignment: .center, spacing: Spacing.m) {
                CartridgeArtwork(cartridge: launcher, size: 56)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Dotty system").font(.lpTitle).foregroundStyle(Color.ink)
                    Text(current.map { newer ? "Update · \($0) → \(launcher.version)" : "Up to date · v\($0)" }
                         ?? "Version \(launcher.version) available")
                        .font(.lpCaption)
                        .foregroundStyle(newer ? DottyLight.leaf.color : Color.inkMuted)
                }
                Spacer(minLength: Spacing.s)
                if newer {
                    Button("Update") { Task { await updateSystem(launcher) } }
                        .buttonStyle(.light(DottyLight.leaf.color))
                        .disabled(install != nil && install?.error == nil)
                        .needsDotty(link)
                }
            }
            Text("Installs and switches cartridges, and runs Wi-Fi, Bluetooth and the lock screen. Updating keeps your cartridges and their files. If a new version ever fails to start, Dotty puts the previous one back by itself.")
                .font(.lpCallout)
                .foregroundStyle(Color.inkMuted)
                .fixedSize(horizontal: false, vertical: true)
            // An update that went wrong while the app wasn't watching (Dotty keeps the note).
            if install == nil, let result = link.info?.update, result.failed, let current {
                NoticeCard(kind: .error, text: Self.explain(result, now: current))
                Button("OK") { Task { _ = try? await link.send("launcher.updateSeen") } }
                    .buttonStyle(.quiet())
            }
        }
        .padding(Spacing.l)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassSurface()
    }

    /// What Dotty's notes about a failed system update mean, in plain words.
    static func explain(_ result: DottyInfo.UpdateResult, now: String) -> String {
        if result.status == "rolledBack" {
            return "System \(result.version) didn't start, so Dotty went back to \(now) by itself. Nothing was lost."
        }
        let why = result.reason.map { ": \($0)" } ?? ""
        return "Dotty couldn't install system \(result.version)\(why). It kept \(now)."
    }

    /// Dotty downloads the new launcher to its SD card and restarts into Rescue, which writes
    /// it and starts it on trial (firmware: cartridges/rescue). The cartridge slot isn't
    /// touched, so the cartridge that was running is simply started again.
    private func updateSystem(_ launcher: CatalogCartridge) async {
        install = InstallState(cartridge: launcher)
        guard let info = link.info else { return }
        let wasRunning = info.isLauncher ? nil : info.name
        do {
            install?.stage = "Switching to the launcher"
            try await link.ensureLauncher()
            install?.stage = "Checking Dotty's Wi-Fi"
            let saved = try await link.send("wifi.list")["networks"] as? [String] ?? []
            guard !saved.isEmpty else {
                install?.error = "Dotty has no Wi-Fi yet. Add a network under Wi-Fi first."
                return
            }
            install?.stage = "Connecting to Wi-Fi"
            do {
                try await link.send("library.fetch", ["id": launcher.id, "name": "Dotty system", "version": launcher.version,
                                                      "size": launcher.size, "sha256": launcher.sha256], timeout: 300)
            } catch {
                // Bluetooth dropping isn't the end: Dotty carries on by itself, so wait for it.
                guard link.lostConnection(error) else {
                    install?.error = "The update didn't download: \(error.localizedDescription). Dotty kept its current system."
                    return
                }
            }
            install?.stage = "Rescue is installing the new system"
            install?.progress = nil
            // Done when the new launcher has confirmed itself, or Dotty reports a failure.
            let deadline = Date().addingTimeInterval(180)
            while true {
                if link.connection == .connected, let now = link.info, now.isLauncher, let result = now.update,
                   result.version == launcher.version {
                    if result.failed {
                        install?.error = Self.explain(result, now: now.version)
                        _ = try? await link.send("launcher.updateSeen")
                        return
                    }
                    if result.status == "updated" { break }
                }
                guard Date() < deadline else {
                    install?.error = "Dotty didn't come back within 3 minutes. Its screen says what happened; it keeps a working system either way."
                    return
                }
                try await Task.sleep(for: .seconds(1))
            }
            _ = try? await link.send("launcher.updateSeen")
            if let wasRunning {
                install?.stage = "Starting \(wasRunning) again"
                try await link.send("launcher.start")
                try await link.waitForReconnect()
            }
            install = nil
            await loadCard()
            success = "Dotty system \(launcher.version) is installed."
            try? await Task.sleep(for: .seconds(2.5))
            success = nil
        } catch {
            install?.error = error.localizedDescription
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
            guard let id = item["id"] as? String, id != "launcher" else { continue }
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
