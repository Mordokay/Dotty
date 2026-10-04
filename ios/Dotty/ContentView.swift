import SwiftUI

/// Pairing first; the dashboard once a Dotty is paired. Every time the app comes to the
/// foreground it reconnects to the paired Dotty (it links up as soon as Dotty advertises).
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
    }
}

#Preview {
    ContentView()
}
