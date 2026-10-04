import SwiftUI

struct ContentView: View {
    var body: some View {
        Group {
            if ProcessInfo.processInfo.arguments.contains("-emptyField") {
                // Debug: the light field alone, to check how the fireflies are spread.
                LightField { Color.clear }
            } else {
                ShowcaseView()
            }
        }
        #if DEBUG
        .overlay(alignment: .topTrailing) {
            FrameRateBadge()
                .padding(.trailing, 20)
                .padding(.top, 2)
        }
        #endif
    }
}

#Preview {
    ContentView()
}
