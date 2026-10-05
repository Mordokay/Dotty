import SwiftUI

/// Wi-Fi / Bluetooth signal as the SF Symbol's variable fill.
struct SignalStrength: View {
    let rssi: Int
    var symbol = "wifi"

    /// -90 dBm → 0, -50 dBm → 1.
    private var level: Double { min(1, max(0, Double(rssi + 90) / 40)) }

    var body: some View {
        Image(systemName: symbol, variableValue: level)
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(Color.inkMuted)
            .accessibilityLabel("Signal \(Int(level * 100))%")
    }
}

/// A glowing progress bar on glass.
struct LightProgress: View {
    let value: Double  // 0...1
    var light: Color = DottyLight.firefly.color

    var body: some View {
        GeometryReader { proxy in
            ZStack(alignment: .leading) {
                Capsule().fill(Color.glassStrong)
                Capsule()
                    .fill(light)
                    .frame(width: max(12, proxy.size.width * min(1, max(0, value))))
                    .shadow(color: light.opacity(0.7), radius: 8)
                    .animation(.settle, value: value)
            }
        }
        .frame(height: 10)
        .accessibilityValue("\(Int(value * 100)) percent")
    }
}

/// A short message on glass: a hint, a success or an error.
struct NoticeCard: View {
    enum Kind { case info, success, error }
    let kind: Kind
    let text: String

    private var light: Color {
        switch kind {
        case .info: DottyLight.lagoon.color
        case .success: DottyLight.leaf.color
        case .error: DottyLight.ember.color
        }
    }

    private var symbol: String {
        switch kind {
        case .info: "info.circle"
        case .success: "checkmark.circle"
        case .error: "exclamationmark.triangle"
        }
    }

    var body: some View {
        HStack(alignment: .top, spacing: Spacing.m) {
            Image(systemName: symbol)
                .font(.system(size: 17, weight: .semibold, design: .rounded))
                .foregroundStyle(light)
            Text(text)
                .font(.lpCallout)
                .foregroundStyle(Color.ink)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
        }
        .padding(Spacing.l)
        .glassSurface(light: light)
    }
}

/// Screen title + subtitle, the way every page starts.
struct PageHeader: View {
    let title: String
    var subtitle: String?

    var body: some View {
        VStack(alignment: .leading, spacing: Spacing.s) {
            Text(title)
                .font(.lpDisplayL)
                .foregroundStyle(Color.ink)
            if let subtitle {
                Text(subtitle)
                    .font(.lpBody)
                    .foregroundStyle(Color.inkMuted)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

/// A cartridge's picture: the full-colour artwork from the catalog, or its e-paper icon
/// (pixel art) while that loads or when the cartridge has none.
struct CartridgeArtwork: View {
    let cartridge: CatalogCartridge
    var size: CGFloat = 72

    var body: some View {
        AsyncImage(url: cartridge.artworkURL, transaction: Transaction(animation: .settle)) { phase in
            if let image = phase.image {
                image.resizable().scaledToFill()
            } else {
                fallback
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: size * 0.24, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: size * 0.24, style: .continuous).strokeBorder(Color.glassEdge, lineWidth: 1))
    }

    private var fallback: some View {
        ZStack {
            Color.glassStrong
            if let icon = cartridge.iconImage() {
                icon.resizable().padding(size * 0.11)
            } else {
                Image(systemName: "square.stack.3d.up").font(.system(size: size * 0.4))
            }
        }
    }
}

/// Shown from the live connection state, so it goes away by itself the moment Dotty
/// reconnects: "Dotty disconnected" (red) for a few seconds after the link drops, then
/// "Searching for Dotty" (blue) while the app keeps trying. Screens show this instead of
/// storing connection errors, and disable what needs Dotty meanwhile.
struct NotConnectedNotice: View {
    @Environment(DottyLink.self) private var link

    static let alertSeconds: TimeInterval = 5

    var body: some View {
        if link.connection != .connected {
            TimelineView(.periodic(from: .now, by: 1)) { context in
                let justDropped = link.lastDisconnect.map { context.date.timeIntervalSince($0) < Self.alertSeconds } ?? false
                Group {
                    if justDropped {
                        NoticeCard(kind: .error, text: "Dotty disconnected.")
                    } else if link.radio == .off {
                        NoticeCard(kind: .info, text: "Bluetooth is off on this iPhone. Turn it on to reach Dotty.")
                    } else {
                        NoticeCard(kind: .info, text: "Searching for Dotty… If it's locked or asleep, press PWR on Dotty.")
                    }
                }
                .animation(.settle, value: justDropped)
            }
            .transition(.opacity)
        }
    }
}

extension View {
    /// Dims and disables controls that need Dotty while it isn't connected.
    func needsDotty(_ link: DottyLink) -> some View {
        disabled(link.connection != .connected)
            .opacity(link.connection == .connected ? 1 : 0.45)
            .animation(.settle, value: link.connection)
    }
}

extension DottyLink {
    /// True when a command failed only because Dotty went away; the live
    /// NotConnectedNotice covers that, so don't keep it as an error.
    func lostConnection(_ error: Error) -> Bool {
        if case DottyError.notConnected = error { return true }
        return connection != .connected
    }
}
