import SwiftUI

@main struct MyApp: App {
    init() {
        FontRegistration.registerBundledFonts()
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
