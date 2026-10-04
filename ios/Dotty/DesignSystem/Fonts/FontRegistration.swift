import CoreText
import Foundation

/// Registers the fonts bundled with the app (Quicksand) so `Font.custom` can find them.
enum FontRegistration {
    static func registerBundledFonts() {
        for url in Bundle.main.urls(forResourcesWithExtension: "ttf", subdirectory: nil) ?? [] {
            CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
        }
    }
}
