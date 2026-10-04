import SwiftUI

/// Home once paired: Dotty's status, cartridges, Wi-Fi and settings.
struct DashboardView: View {
    @Environment(DottyLink.self) private var link
    @State private var path: [Route] = []
    @State private var confirmForget = false
    @State private var showDesignSystem = false

    enum Route: Hashable {
        case cartridges, wifi, music

        /// The screen of the cartridge Dotty is running, if it has one (and its firmware
        /// has the commands that screen needs).
        static func screen(for info: DottyInfo?) -> Route? {
            guard let info, !info.isLauncher else { return nil }
            switch info.id {
            case "music" where info.isAtLeast("0.7.0"): return .music  // library + playlists
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
                                     systemImage: "square.stack.3d.up", action: { path.append(.cartridges) })
                        }
                        GlassCard(title: "Wi-Fi") {
                            LightRow(title: "Wi-Fi networks", subtitle: "Networks Dotty can join",
                                     systemImage: "wifi", action: { path.append(.wifi) })
                        }
                        GlassCard(title: "Settings") {
                            LightRow(title: "Forget this Dotty", systemImage: "xmark.circle",
                                     action: { confirmForget = true })
                            LightRow(title: "Design system", systemImage: "paintpalette",
                                     action: { showDesignSystem = true })
                        }
                    }
                    .padding(.horizontal, Spacing.l)
                    .padding(.bottom, Spacing.xxl)
                }
                .refreshable { link.refreshInfo() }
            }
            .toolbar(.hidden, for: .navigationBar)
            .navigationDestination(for: Route.self) { route in
                switch route {
                case .cartridges: CartridgesView()
                case .wifi: WiFiView()
                case .music: MusicView()
                }
            }
        }
        .confirmationDialog("Forget this Dotty?", isPresented: $confirmForget, titleVisibility: .visible) {
            Button("Forget", role: .destructive) { Task { await link.forget() } }
        } message: {
            Text("Dotty forgets this iPhone. Also remove it under Settings › Bluetooth to pair again.")
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
                LightRow(title: "Battery", systemImage: batterySymbol(info.battery)) {
                    Text("\(info.battery)%")
                }
            } else {
                LightRow(title: "Not connected", subtitle: "Press PWR on Dotty to wake it.",
                         systemImage: "moon.zzz")
            }
            if let serial = link.paired?.serial {
                LightRow(title: "Serial", systemImage: "number") {
                    Text(serial).font(.lpCaption.monospaced())
                }
            }
            HStack {
                Spacer()
                LightOrb(color: DottyLight.firefly.color, size: 72, pulse: link.connection == .connected)
                    .opacity(link.connection == .connected ? 1 : 0.35)
                    .padding(.vertical, Spacing.l)
                Spacer()
            }
        }
    }

    private func screenSubtitle(_ route: Route) -> String {
        route == .music ? "Playing, playlists and sending songs" : ""
    }

    private func screenSymbol(_ route: Route) -> String {
        route == .music ? "music.note.list" : "square.stack.3d.up"
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
