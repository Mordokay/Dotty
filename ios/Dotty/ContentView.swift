import SwiftUI

/// Pairing first; the dashboard once a Dotty is paired. Every time the app comes to the
/// foreground it reconnects to the paired Dotty (it links up as soon as Dotty advertises),
/// and while it stays open it keeps Dotty awake: any command restarts Dotty's 2-minute
/// auto-lock, and locking turns Bluetooth off, so a quiet screen would lose Dotty.
struct ContentView: View {
    @State private var link = DottyLink()
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        Group {
            if link.paired == nil {
                WelcomeView()
            } else {
                DashboardView()
            }
        }
        .environment(link)
        .preferredColorScheme(.dark)
        .animation(.settle, value: link.paired)
        .onChange(of: scenePhase) { _, phase in
            if phase == .active { link.reconnect() }
        }
        // Keep-alive: a ping every 45 s, only while the app is in front and connected (in the
        // background, or once the iPhone locks, Dotty's own auto-lock takes over again).
        .task(id: scenePhase == .active && link.connection == .connected) {
            guard scenePhase == .active, link.connection == .connected else { return }
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(45))
                guard !Task.isCancelled else { return }
                _ = try? await link.send("core.ping")
            }
        }
        // Watchdog: while the app is in front, make sure a connection attempt is under way.
        .task(id: scenePhase == .active) {
            guard scenePhase == .active else { return }
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(10))
                link.nudge()
            }
        }
    }
}

#Preview {
    ContentView()
}
