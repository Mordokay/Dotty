import SwiftUI

/// The catalog from GitHub. Installing asks Dotty to fetch the cartridge over Wi-Fi onto its
/// SD card (skipped if it's already there) and boot it.
struct CartridgesView: View {
    @Environment(DottyLink.self) private var link
    @State private var catalog: Catalog?
    @State private var loadError: String?
    @State private var install: InstallState?

    struct InstallState {
        var cartridge: CatalogCartridge
        var stage = "Getting ready"
        var progress: Double?
        var error: String?
        var done = false
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Cartridges",
                               subtitle: "Dotty downloads them over Wi-Fi and keeps a copy on its SD card, so switching back is quick.")
                        .padding(.top, Spacing.l)
                    if let install { installCard(install) }
                    if let loadError { NoticeCard(kind: .error, text: loadError) }
                    if let catalog {
                        ForEach(catalog.cartridges) { cartridge in cartridgeCard(cartridge) }
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
        .task { await load() }
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
        let running = link.info.map { !$0.isLauncher && $0.id == cartridge.id && $0.version == cartridge.version } ?? false
        return GlassCard {
            HStack(alignment: .top, spacing: Spacing.l) {
                Group {
                    if let icon = cartridge.iconImage() {
                        icon.resizable().frame(width: 56, height: 56)
                    } else {
                        Image(systemName: "square.stack.3d.up").font(.system(size: 28))
                    }
                }
                .frame(width: 72, height: 72)
                .background(RoundedRectangle(cornerRadius: Radius.soft).fill(Color.glassStrong))

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
                    HStack {
                        if running {
                            StatusPill(state: .connected, text: "Running")
                        } else {
                            Button("Install") { Task { await installCartridge(cartridge) } }
                                .buttonStyle(.light())
                                .disabled(install != nil && install?.done == false && install?.error == nil)
                        }
                    }
                    .padding(.top, Spacing.s)
                }
            }
            .padding(Spacing.m)
        }
    }

    private func installCard(_ state: InstallState) -> some View {
        GlassCard(title: state.cartridge.name, light: DottyLight.firefly.color) {
            VStack(alignment: .leading, spacing: Spacing.m) {
                if let error = state.error {
                    Text(error).font(.lpCallout).foregroundStyle(DottyLight.ember.color)
                    Button("Close") { install = nil }.buttonStyle(.quiet())
                } else if state.done {
                    Text("Installed. Dotty restarted into \(state.cartridge.name).")
                        .font(.lpCallout).foregroundStyle(Color.ink)
                    Button("Done") { install = nil }.buttonStyle(.light(DottyLight.leaf.color))
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

    private func installCartridge(_ cartridge: CatalogCartridge) async {
        install = InstallState(cartridge: cartridge)
        do {
            install?.stage = "Checking Dotty's Wi-Fi"
            let saved = try await link.send("wifi.list")["networks"] as? [String] ?? []
            guard !saved.isEmpty else {
                install?.error = "Dotty has no Wi-Fi yet. Add a network under Wi-Fi first."
                return
            }
            install?.stage = "Switching to the launcher"
            try await link.ensureLauncher()
            install?.stage = "Connecting to Wi-Fi"
            // Name/version/size let Dotty's screen show the cartridge before the catalog arrives.
            try await link.send("library.fetch", ["id": cartridge.id, "name": cartridge.name, "version": cartridge.version,
                                                  "size": cartridge.size, "sha256": cartridge.sha256], timeout: 300)
            install?.stage = "Restarting"
            install?.progress = nil
            try await link.waitForReconnect()
            install?.done = true
        } catch {
            install?.error = error.localizedDescription
        }
    }
}
