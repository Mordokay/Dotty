import SwiftUI

/// Home once paired: Dotty's status, cartridges, Wi-Fi and settings.
struct DashboardView: View {
    @Environment(DottyLink.self) private var link
    /// A NavigationPath, not [Route]: pushed screens add their own destinations (a Music
    /// playlist), and a typed path silently ignores links to anything but Route.
    @State private var path = NavigationPath()
    @State private var confirmForget = false
    @State private var confirmReset = false
    /// The factory reset in progress (what it's doing), or its error.
    @State private var resetStage: String?
    @State private var resetError: String?
    @State private var showDesignSystem = false
    @State private var wifi: WiFiState?

    enum Route: Hashable {
        case cartridges, wifi, music, jokes, weather, album, tape, news, pet, backups

        /// The screen of the cartridge Dotty is running, if it has one (and its firmware
        /// has the commands that screen needs).
        static func screen(for info: DottyInfo?) -> Route? {
            guard let info, !info.isLauncher else { return nil }
            switch info.id {
            case "music" where info.isAtLeast("0.7.0"): return .music  // library + playlists
            case "jokes" where info.supports("jokes"): return .jokes
            case "weather" where info.supports("weather"): return .weather
            case "album" where info.supports("album"): return .album
            case "tape" where info.supports("tape"): return .tape
            case "news" where info.supports("news"): return .news
            case "pet" where info.supports("pet"): return .pet
            default: return nil
            }
        }
    }

    var body: some View {
        NavigationStack(path: $path) {
            LightField {
                ScrollView {
                    VStack(alignment: .leading, spacing: Spacing.xl) {
                        header
                        if let resetStage {
                            GlassCard(title: "Factory reset", light: DottyLight.ember.color) {
                                HStack { Spacer(); FireflyLoader(size: 64, label: resetStage); Spacer() }
                                Text(resetStage).font(.lpCallout).foregroundStyle(Color.ink).padding(Spacing.m)
                            }
                        }
                        if let resetError { NoticeCard(kind: .error, text: resetError) }
                        NotConnectedNotice()
                        dottyCard
                        if let route = Route.screen(for: link.connection == .connected ? link.info : nil),
                           let info = link.info {
                            GlassCard(title: info.name, light: DottyLight.firefly.color) {
                                LightRow(title: "Open \(info.name)", subtitle: screenSubtitle(route),
                                         systemImage: screenSymbol(route), action: { path.append(route) })
                            }
                        }
                        GlassCard(title: "Cartridges") {
                            LightRow(title: "Browse cartridges", subtitle: "Install one over Wi-Fi",
                                     systemImage: "square.stack.3d.up", action: { path.append(Route.cartridges) })
                        }
                        GlassCard(title: "Wi-Fi") {
                            LightRow(title: "Wi-Fi networks", subtitle: wifi?.summary ?? "Networks Dotty can join",
                                     systemImage: "wifi", action: { path.append(Route.wifi) }) {
                                if let rssi = wifi?.current?.rssi ?? wifi?.last?.rssi { SignalStrength(rssi: rssi) }
                            }
                        }
                        GlassCard(title: "Settings") {
                            LightRow(title: "Forget this Dotty", systemImage: "xmark.circle",
                                     action: { confirmForget = true })
                            LightRow(title: "Backups", subtitle: "Copy Dotty's SD card to this iPhone, or put a copy back",
                                     systemImage: "externaldrive", action: { path.append(Route.backups) })
                            LightRow(title: "Factory reset", subtitle: "Erase everything and start fresh",
                                     systemImage: "arrow.counterclockwise.circle", action: { confirmReset = true })
                                .needsDotty(link)
                                .disabled(resetStage != nil)
                            LightRow(title: "Design system", systemImage: "paintpalette",
                                     action: { showDesignSystem = true })
                        }
                    }
                    .padding(.horizontal, Spacing.l)
                    .padding(.bottom, Spacing.xxl)
                }
                .refreshable {
                    link.refreshInfo()
                    await loadWiFi()
                }
            }
            .toolbar(.hidden, for: .navigationBar)
            .navigationDestination(for: Route.self) { route in
                switch route {
                case .cartridges: CartridgesView()
                case .wifi: WiFiView()
                case .music: MusicView()
                case .jokes: JokesView()
                case .weather: WeatherView()
                case .album: AlbumView()
                case .tape: TapeView()
                case .news: NewsView()
                case .pet: PetView()
                case .backups: BackupView()
                }
            }
        }
        // On connect (and back from the Wi-Fi screen): which network Dotty uses.
        .task(id: wifiReloadKey) { await loadWiFi() }
        .confirmationDialog("Forget this Dotty?", isPresented: $confirmForget, titleVisibility: .visible) {
            Button("Forget", role: .destructive) { Task { await link.forget() } }
        } message: {
            Text("Dotty forgets this iPhone. Also remove it under Settings › Bluetooth to pair again.")
        }
        .confirmationDialog("Erase everything on Dotty?", isPresented: $confirmReset, titleVisibility: .visible) {
            Button("Erase everything", role: .destructive) { Task { await factoryReset() } }
        } message: {
            Text("The SD card is formatted (songs, photos, recordings and every file on it), and every setting goes: Wi-Fi networks, Bluetooth pairings and each cartridge's settings. Dotty keeps only its system, updated to the newest if it can. This can't be undone.")
        }
        .fullScreenCover(isPresented: $showDesignSystem) {
            ShowcaseView()
                .overlay(alignment: .topTrailing) {
                    Button { showDesignSystem = false } label: { Image(systemName: "xmark") }
                        .buttonStyle(.frostedCircle)
                        .padding(Spacing.l)
                }
        }
    }

    private var header: some View {
        HStack {
            DottyLogo(size: 44, horizontal: true)
            Spacer()
            StatusPill(state: pillState)
        }
        .padding(.top, Spacing.l)
    }

    private var pillState: StatusPill.State {
        switch link.connection {
        case .connected: .connected
        case .connecting: .searching
        case .idle: .off
        }
    }

    private var dottyCard: some View {
        GlassCard(title: link.paired?.name ?? "Dotty", light: DottyLight.firefly.color) {
            if let info = link.info, link.connection == .connected {
                // Cartridges with their own screen open it from here.
                LightRow(title: info.cartridgeName ?? "No cartridge",
                         subtitle: info.isLauncher ? "In the launcher" : "Running now",
                         systemImage: "square.stack.3d.up",
                         action: Route.screen(for: info).map { route in { path.append(route) } })
                LightRow(title: "Battery",
                         subtitle: info.charging == true ? "Charging" : (info.power == true ? "Fully charged" : nil),
                         systemImage: info.charging == true ? "battery.100percent.bolt" : batterySymbol(info.battery)) {
                    Text("\(info.battery)%")
                }
            } else {
                LightRow(title: "Not connected", systemImage: "moon.zzz")  // the notice above explains
            }
            if let serial = link.paired?.serial {
                LightRow(title: "Serial", systemImage: "number") {
                    Text(serial).font(.lpCaption.monospaced())
                }
            }
        }
    }

    private func screenSubtitle(_ route: Route) -> String {
        switch route {
        case .music: "Playing, playlists and sending songs"
        case .jokes: "Favourites and the jokes on Dotty"
        case .weather: "Location and units"
        case .album: "Photos, albums and the lock screen"
        case .tape: "Your recordings: listen, share, rename"
        case .news: "Your topics and favourites"
        case .pet: "How it's doing, pause and sounds"
        case .backups: "Copies of Dotty's SD card"
        default: ""
        }
    }

    private func screenSymbol(_ route: Route) -> String {
        switch route {
        case .music: "music.note.list"
        case .jokes: "face.smiling"
        case .weather: "cloud.sun"
        case .album: "photo.on.rectangle"
        case .tape: "recordingtape"
        case .news: "newspaper"
        case .pet: "pawprint"
        case .backups: "externaldrive"
        default: "square.stack.3d.up"
        }
    }

    private var wifiReloadKey: String {
        "\(link.connection == .connected)-\(link.info?.id ?? "")-\(path.count)"
    }

    /// Rescue erases Dotty (firmware: launcher.factoryReset → cartridges/rescue). First, while
    /// Dotty still knows the Wi-Fi, it fetches the newest system so that's the one it keeps.
    private func factoryReset() async {
        resetError = nil
        do {
            resetStage = "Switching to the launcher"
            try await link.ensureLauncher()
            var newer: String?
            if let catalog = try? await Catalog.load(), let launcher = catalog.launcher,
               let current = link.info?.launcherVersion,
               launcher.version.compare(current, options: .numeric) == .orderedDescending,
               let saved = try? await link.send("wifi.list")["networks"] as? [String], !saved.isEmpty {
                resetStage = "Downloading the newest system"
                if (try? await link.send("library.fetch", ["id": launcher.id, "name": "Dotty system", "version": launcher.version,
                                                           "size": launcher.size, "sha256": launcher.sha256, "install": false],
                                         timeout: 300)) != nil {
                    newer = launcher.version
                }
            }
            resetStage = "Erasing Dotty"
            try await link.send("launcher.factoryReset", newer.map { ["launcher": $0] } ?? [:])
            resetStage = nil
            link.forgetAfterReset()  // the pairing screen takes over
        } catch {
            resetStage = nil
            resetError = "Couldn't start the factory reset: \(error.localizedDescription)"
        }
    }

    private func loadWiFi() async {
        guard link.connection == .connected, link.info?.supports("wifi") == true else { return }
        wifi = try? await WiFiState.load(from: link)
    }

    private func batterySymbol(_ percent: Int) -> String {
        switch percent {
        case ..<13: "battery.0percent"
        case ..<38: "battery.25percent"
        case ..<63: "battery.50percent"
        case ..<88: "battery.75percent"
        default: "battery.100percent"
        }
    }
}
