import SwiftUI

/// Every design system component on one screen, lit in the bracelet's colour.
/// Picking a colour re-tints the whole screen, as the bracelet's colour will in the app.
struct ShowcaseView: View {
    @State private var bracelet: DottyLight = .firefly
    /// Scrolling writes here without re-rendering this screen; the light field reads it each frame.
    @State private var scroll = ScrollProbe()
    @State private var notifications = true
    @State private var lightWhileDisconnected = false
    @State private var vibration = 80.0
    @State private var brightness = 45.0
    @State private var glow = 70.0
    @State private var wheelColor = DottyLight.lagoon.color
    @State private var jellyRichness = 100.0
    @State private var jellyOpacity = 60.0

    private var jellyMaterial: JellyMaterial {
        JellyMaterial(richness: jellyRichness / 100, opacity: jellyOpacity / 100)
    }
    // Launch with -jellyLab to open straight into the jelly lab.
    @State private var showJellyLab = ProcessInfo.processInfo.arguments.contains("-jellyLab")

    var body: some View {
        NavigationStack {
            LightField(lights: [bracelet.color, DottyLight.lagoon.color, DottyLight.bloom.color], scroll: scroll) {
                ScrollView {
                    VStack(alignment: .leading, spacing: Spacing.xl) {
                        header
                        braceletCard
                        section("Accent colour") {
                            LightColorPicker(selection: $bracelet)
                                .padding(.vertical, Spacing.s)
                        }
                        section("Actions") { actions }
                        section("Controls") { controls }
                        section("Rows") { rows }
                        section("Status") { status }
                        section("Light") { lights }
                        section("Coming next") {
                            Button("Open the jelly lab") { showJellyLab = true }
                                .buttonStyle(.light(bracelet.color))
                        }
                    }
                    .padding(.horizontal, Spacing.l)
                    .padding(.top, Spacing.l)
                    .padding(.bottom, Spacing.xxxl)
                }
                .scrollIndicators(.hidden)
                // Launch with -scrollToControls or -scrollToEnd to open further down (for screenshots).
                .defaultScrollAnchor(Self.launchAnchor)
                .onScrollGeometryChange(for: CGFloat.self) { geometry in
                    geometry.contentOffset.y + geometry.contentInsets.top
                } action: { _, offset in
                    scroll.offset = offset
                }
            }
            .animation(.settle, value: bracelet)
            .environment(\.sliderSparks, true)
            .toolbar(.hidden, for: .navigationBar)
            .navigationDestination(isPresented: $showJellyLab) {
                JellyLabView(light: bracelet)
            }
        }
        .preferredColorScheme(.dark)
    }

    /// The painted rim on the Rows panel: a strong blue, to compare with real rim lighting.
    static let strongBlue = Color(hex: 0x2F6BFF)

    private static var launchAnchor: UnitPoint {
        let arguments = ProcessInfo.processInfo.arguments
        if arguments.contains("-scrollToEnd") { return .bottom }
        if arguments.contains("-scrollToControls") { return .center }
        if arguments.contains("-scrollToActions") { return UnitPoint(x: 0.5, y: 0.22) }
        return .top
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: Spacing.s) {
            DottyLogo(size: 64, horizontal: true)
            Text("Light, colour and touch. Every component of the design system, lit in your accent colour.")
                .font(.lpBody)
                .foregroundStyle(Color.inkMuted)
        }
    }

    private var braceletCard: some View {
        GlassCard(title: "Your Dotty", light: bracelet.color) {
            LightRow(title: "Dotty", subtitle: "Music cartridge 0.4.0 · Battery 84%", light: bracelet.color) {
                StatusPill(state: .connected, light: bracelet.color)
            }
            HStack {
                Spacer()
                LightOrb(color: bracelet.color, size: 88, pulse: true)
                    .padding(.vertical, Spacing.xl)
                Spacer()
            }
        }
    }

    private var actions: some View {
        VStack(alignment: .leading, spacing: Spacing.l) {
            // Jelly glass: lit in the bracelet's colour, clear, and two moving gradients. All four
            // share one world, so they push each other.
            JellyCluster(size: CGSize(width: 360, height: 150), material: jellyMaterial, specs: [
                JellySpec(id: "send", light: bracelet.color, size: CGSize(width: 170, height: 60), center: CGPoint(x: 88, y: 36),
                          label: AnyView(Label("Send touch", systemImage: "hand.tap").font(.lpHeadline)), action: {}),
                JellySpec(id: "choose", light: Color(white: 0.92), size: CGSize(width: 170, height: 60), center: CGPoint(x: 272, y: 36),
                          label: AnyView(Text("Choose colour").font(.lpHeadline)), action: {}),
                JellySpec(id: "light-up", light: DottyLight.dusk.color, size: CGSize(width: 170, height: 60), center: CGPoint(x: 88, y: 114),
                          label: AnyView(Text("Light up").font(.lpHeadline)), action: {},
                          gradient: [DottyLight.lagoon.color, DottyLight.dusk.color, DottyLight.bloom.color, DottyLight.ember.color]),
                JellySpec(id: "glow", light: DottyLight.leaf.color, size: CGSize(width: 170, height: 60), center: CGPoint(x: 272, y: 114),
                          label: AnyView(Label("Glow", systemImage: "sparkles").font(.lpHeadline)), action: {},
                          gradient: [DottyLight.firefly.color, DottyLight.leaf.color, DottyLight.lagoon.color], vertical: true),
            ])

            VStack(spacing: Spacing.m) {
                LightSlider(title: "Jelly richness", value: $jellyRichness, range: 20...100, light: bracelet.color)
                LightSlider(title: "Jelly opacity", value: $jellyOpacity, light: bracelet.color,
                            emptyLight: bracelet.color.withRichness(0.15))
            }
            .padding(Spacing.l)
            .glassSurface()

            // Buttons that turn into something else, stretching to their new size like jelly.
            HStack(spacing: Spacing.m) {
                MorphButton(faces: [
                    MorphFace(light: DottyLight.leaf.color, title: "KIWI"),
                    MorphFace(light: DottyLight.ember.color, title: "STRAWBERRY"),
                ])
                MorphButton(faces: [
                    MorphFace(light: DottyLight.dusk.color, systemImage: "moon.stars.fill"),
                    MorphFace(light: DottyLight.amber.color, title: "Rise and shine", systemImage: "sun.max.fill"),
                ])
            }
            .frame(height: 60)

            HStack(spacing: Spacing.m) {
                Button { } label: { Image(systemName: "play.fill") }
                    .buttonStyle(.light(bracelet.color, circle: true))
                    .accessibilityLabel("Play pattern")
                Button { } label: { Image(systemName: "stop.fill") }
                    .buttonStyle(.frostedCircle)
                    .accessibilityLabel("Stop")
                Button { } label: { Image(systemName: "plus") }
                    .buttonStyle(.frostedCircle)
                    .accessibilityLabel("New pattern")
                Button("Not now") {}
                    .buttonStyle(.quiet(bracelet.color))
            }
        }
    }

    private var controls: some View {
        VStack(spacing: Spacing.l) {
            Toggle("Notifications on Dotty", isOn: $notifications)
                .toggleStyle(.light(bracelet.color))
            Toggle("Light while disconnected", isOn: $lightWhileDisconnected)
                .toggleStyle(.light(bracelet.color))
            LightSlider(title: "Vibration", value: $vibration, light: DottyLight.ember.color, emptyLight: DottyLight.lagoon.color)
            LightSlider(title: "Brightness", value: $brightness, light: DottyLight.firefly.color, emptyLight: DottyLight.dusk.color)
            LightSlider(title: "Glow", value: $glow, light: bracelet.color)
        }
        .padding(Spacing.l)
        .glassSurface()
    }

    private var rows: some View {
        GlassCard(title: "Saved patterns", rim: .painted(Self.strongBlue)) {
            LightRow(title: "Heartbeat", subtitle: "4 touches · 2.1 s", light: DottyLight.bloom.color, action: {})
            LightRow(title: "Good night", subtitle: "2 touches · 1.4 s", light: DottyLight.dusk.color, action: {})
            LightRow(title: "Vibration", systemImage: "waveform") {
                Text("\(Int(vibration))%")
            }
        }
    }

    private var status: some View {
        VStack(alignment: .leading, spacing: Spacing.s) {
            HStack(spacing: Spacing.s) {
                StatusPill(state: .connected, light: bracelet.color)
                StatusPill(state: .searching)
            }
            HStack(spacing: Spacing.s) {
                StatusPill(state: .connecting)
                StatusPill(state: .off)
            }
            StatusPill(state: .error)
        }
    }

    private var lights: some View {
        VStack(spacing: Spacing.xl) {
            FireflyColorWheel(color: $wheelColor, size: 400)
                .frame(maxWidth: .infinity)
                .padding(.horizontal, -Spacing.l)
            HStack(spacing: Spacing.xl) {
                FireflyLoader(size: 90, light: wheelColor, label: "Looking for your Dotty")
                Text("Drag around the ring to light the firefly. The swelling follows your finger and grows the faster you go.")
                    .font(.lpCallout)
                    .foregroundStyle(Color.inkMuted)
            }
        }
    }

    private func section<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: Spacing.m) {
            Text(title.uppercased())
                .font(.lpLabel)
                .tracking(0.7)
                .foregroundStyle(Color.inkMuted)
            content()
        }
        // Anything animating in this section stops while it is scrolled out of view.
        .pausesOffscreen()
    }
}

#Preview {
    ShowcaseView()
}
