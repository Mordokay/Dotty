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
