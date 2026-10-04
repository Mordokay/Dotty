import SwiftUI

/// One row inside a GlassCard: an optional light dot (anything with a colour: a bracelet, a
/// partner, a pattern) or an SF Symbol, a title, a subtitle and a trailing value. With an action
/// it becomes a button with a chevron.
struct LightRow<Trailing: View>: View {
    let title: String
    var subtitle: String?
    var light: Color?
    var systemImage: String?
    var action: (() -> Void)?
    @ViewBuilder var trailing: Trailing

    var body: some View {
        if let action {
            Button(action: action) { row(chevron: true) }
                .buttonStyle(RowButtonStyle())
        } else {
            row(chevron: false)
        }
    }

    private func row(chevron: Bool) -> some View {
        HStack(spacing: Spacing.m) {
            if let light {
                LightOrb(color: light, size: 22)
                    .frame(width: 40, height: 40)
            } else if let systemImage {
                Image(systemName: systemImage)
                    .font(.system(size: 17, weight: .semibold, design: .rounded))
                    .foregroundStyle(Color.ink)
                    .frame(width: 40, height: 40)
                    .background(Circle().fill(Color.glassStrong))
            }
            VStack(alignment: .leading, spacing: 0) {
                Text(title).font(.lpHeadline).foregroundStyle(Color.ink)
                if let subtitle {
                    Text(subtitle).font(.lpCaption).foregroundStyle(Color.inkMuted)
                }
            }
            Spacer(minLength: Spacing.s)
            trailing
                .font(.lpCallout)
                .foregroundStyle(Color.inkMuted)
            if chevron {
                Image(systemName: "chevron.right")
                    .font(.system(size: 14, weight: .semibold, design: .rounded))
                    .foregroundStyle(Color.inkMuted)
            }
        }
        .padding(.horizontal, Spacing.m)
        .padding(.vertical, Spacing.s)
        .frame(minHeight: 56)
        .contentShape(Rectangle())
    }
}

extension LightRow where Trailing == EmptyView {
    init(title: String, subtitle: String? = nil, light: Color? = nil, systemImage: String? = nil, action: (() -> Void)? = nil) {
        self.init(title: title, subtitle: subtitle, light: light, systemImage: systemImage, action: action) { EmptyView() }
    }
}

private struct RowButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .background(RoundedRectangle(cornerRadius: 20, style: .continuous).fill(configuration.isPressed ? Color.glass : .clear))
            .sensoryFeedback(trigger: configuration.isPressed) { _, isPressed in isPressed ? Haptic.tap : nil }
    }
}

/// The bracelet's connection at a glance: a small light on glass.
struct StatusPill: View {
    enum State { case connected, searching, connecting, off, error }

    var state: State
    var text: String?
    var light: Color?

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.motionActive) private var active

    private var dotColor: Color? {
        switch state {
        case .connected: light ?? DottyLight.firefly.color
        case .searching: DottyLight.amber.color
        case .connecting: DottyLight.lagoon.color
        case .error: DottyLight.ember.color
        case .off: nil
        }
    }

    private var defaultText: String {
        switch state {
        case .connected: "Connected"
        case .searching: "Searching"
        case .connecting: "Connecting"
        case .off: "Not connected"
        case .error: "Can't connect"
        }
    }

    var body: some View {
        HStack(spacing: Spacing.s) {
            dot
            Text(text ?? defaultText)
                .font(.lpCallout.weight(.semibold))
                .lineLimit(1)
                .fixedSize()
                .foregroundStyle(state == .error ? DottyLight.ember.color : Color.ink)
        }
        .padding(.horizontal, Spacing.m)
        .frame(minHeight: 32)
        .glassSurface(cornerRadius: 16)
    }

    @ViewBuilder private var dot: some View {
        let circle = Circle().fill(dotColor ?? .inkFaint).frame(width: 10, height: 10)
        if let dotColor, !reduceMotion, active, state != .error {
            circle.phaseAnimator([0.0, 1.0]) { content, phase in
                content
                    .scaleEffect(0.85 + 0.3 * phase)
                    .opacity(0.45 + 0.55 * phase)
                    .background { GlowView(color: dotColor, intensity: 0.7, core: 0.4, wobble: false).padding(-7) }
            } animation: { _ in
                .timingCurve(0.45, 0, 0.55, 1, duration: state == .connected ? Durations.pulse / 2 : Durations.pulse / 4)
            }
        } else {
            circle.background {
                if let dotColor { GlowView(color: dotColor, intensity: 0.7, core: 0.4, wobble: false).padding(-7) }
            }
        }
    }
}
