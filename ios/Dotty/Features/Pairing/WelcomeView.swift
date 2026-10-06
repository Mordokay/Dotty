import SwiftUI

/// First run: look for Dottys nearby and pair with one.
struct WelcomeView: View {
    @Environment(DottyLink.self) private var link
    @State private var pairing: DottyLink.Nearby?

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    DottyLogo(size: 64, horizontal: true)
                        .padding(.top, Spacing.xl)
                    PageHeader(title: "Let's find your Dotty",
                               subtitle: "Wake Dotty with a short press on PWR and keep it close to your iPhone.")
                    if let problem = radioProblem {
                        NoticeCard(kind: .error, text: problem)
                    }
                    if let name = UserDefaults.standard.string(forKey: DottyLink.resetNameKey) {
                        // iOS keeps its own pairing with the old Dotty; only the person can remove it.
                        NoticeCard(kind: .info, text: "Dotty was reset. On this iPhone, open Settings › Bluetooth, tap ⓘ next to “\(name)” and choose Forget This Device. Then pair it again below.")
                    }
                    nearbyCard
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
        }
        .onAppear { link.startScanning() }
        .onDisappear { link.stopScanning() }
        .sheet(item: $pairing) { dotty in
            PairingSheet(dotty: dotty)
                .presentationDetents([.medium, .large])
                .presentationBackground(.clear)
        }
    }

    private var nearbyCard: some View {
        GlassCard(title: "Nearby") {
            if link.nearby.isEmpty {
                HStack {
                    Spacer()
                    VStack(spacing: Spacing.m) {
                        FireflyLoader(size: 96, label: "Looking for Dotty")
                        Text("Looking for Dotty…")
                            .font(.lpCallout)
                            .foregroundStyle(Color.inkMuted)
                    }
                    Spacer()
                }
                .padding(.vertical, Spacing.xl)
            } else {
                ForEach(link.nearby.sorted { $0.rssi > $1.rssi }) { dotty in
                    LightRow(title: dotty.name, subtitle: dotty.serial.map { "Serial \($0)" },
                             light: DottyLight.firefly.color, action: { pairing = dotty }) {
                        SignalStrength(rssi: dotty.rssi, symbol: "dot.radiowaves.left.and.right")
                    }
                }
            }
        }
    }

    private var radioProblem: String? {
        switch link.radio {
        case .off: "Bluetooth is off. Turn it on in Control Center to find Dotty."
        case .unauthorized: "Dotty needs Bluetooth access. Allow it in Settings › Dotty."
        case .unsupported: "This device has no Bluetooth LE."
        case .ready, .unknown: nil
        }
    }
}

/// Connect → type the code shown on Dotty (iOS asks for it) → paired.
struct PairingSheet: View {
    let dotty: DottyLink.Nearby

    @Environment(DottyLink.self) private var link
    @Environment(\.dismiss) private var dismiss
    @State private var step: Step = .connecting
    @State private var result: PairedDotty?

    enum Step: Equatable { case connecting, code, paired, failed(String) }

    var body: some View {
        VStack(spacing: Spacing.xl) {
            Spacer(minLength: Spacing.xl)
            switch step {
            case .connecting:
                FireflyLoader(size: 120, label: "Connecting")
                message("Connecting to \(dotty.name)", "Keep Dotty awake and close by.")
            case .code:
                Image(systemName: "number.square")
                    .font(.system(size: 64, weight: .semibold, design: .rounded))
                    .foregroundStyle(DottyLight.firefly.color)
                    .shadow(color: DottyLight.firefly.color.opacity(0.6), radius: 16)
                message("Type the code shown on Dotty",
                        "Dotty shows a 6-digit code on its screen. Enter it when your iPhone asks.")
            case .paired:
                LightOrb(color: DottyLight.leaf.color, size: 96, pulse: true)
                message("Paired with \(dotty.name)", "Dotty is now in your iPhone's Bluetooth settings.")
                Button("Continue") {
                    if let result { link.remember(result) }
                    dismiss()
                }
                .buttonStyle(.light(DottyLight.leaf.color))
            case .failed(let reason):
                Image(systemName: "exclamationmark.triangle")
                    .font(.system(size: 48, weight: .semibold))
                    .foregroundStyle(DottyLight.ember.color)
                message("Couldn't pair", reason)
                HStack(spacing: Spacing.m) {
                    Button("Try again") { Task { await pair() } }
                        .buttonStyle(.light())
                    Button("Cancel") { dismiss() }
                        .buttonStyle(.quiet())
                }
            }
            Spacer(minLength: Spacing.xl)
        }
        .padding(Spacing.xl)
        .frame(maxWidth: .infinity)
        .glassSurface(cornerRadius: Radius.sheet, frosted: true)
        .padding(Spacing.s)
        .task { await pair() }
        .onChange(of: link.connection) { _, connection in
            if connection == .connected, step == .connecting { step = .code }
        }
        .sensoryFeedback(trigger: step) { _, step in
            switch step {
            case .paired: Haptic.confirm
            case .failed: Haptic.error
            default: nil
            }
        }
        .interactiveDismissDisabled(step == .code)
    }

    private func message(_ title: String, _ detail: String) -> some View {
        VStack(spacing: Spacing.s) {
            Text(title)
                .font(.lpTitle)
                .foregroundStyle(Color.ink)
                .multilineTextAlignment(.center)
            Text(detail)
                .font(.lpBody)
                .foregroundStyle(Color.inkMuted)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func pair() async {
        step = .connecting
        do {
            result = try await link.pair(with: dotty)
            step = .paired
        } catch {
            step = .failed(error.localizedDescription)
        }
    }
}
