import SwiftUI
import UIKit

/// The haptic tokens: every light moment has a feel, as on the bracelet.
enum Haptic {
    /// Pressing a button or row.
    static let tap = SensoryFeedback.impact(flexibility: .soft, intensity: 0.5)
    /// Choosing a colour, flipping a toggle, each step of a slider.
    static let select = SensoryFeedback.selection
    /// Paired, saved, sent.
    static let confirm = SensoryFeedback.success
    /// Something failed.
    static let error = SensoryFeedback.error
    /// Each peak of a pulsing light the person is waiting on.
    static let pulse = SensoryFeedback.impact(flexibility: .soft, intensity: 0.3)

    /// A one-off soft impact at a given strength, for continuous moments (a jelly squashing).
    /// Impacts closer together than the Taptic Engine can play (a wheel spun fast) are dropped:
    /// queueing them backs up the engine and slows the app for seconds afterwards.
    @MainActor
    static func impact(_ intensity: Double) {
        let now = CACurrentMediaTime()
        guard now - lastImpact >= minimumImpactGap else { return }
        lastImpact = now
        softImpact.impactOccurred(intensity: max(0.05, min(1, intensity)))
        softImpact.prepare()
    }

    @MainActor private static let softImpact = UIImpactFeedbackGenerator(style: .soft)
    @MainActor private static var lastImpact: CFTimeInterval = 0
    private static let minimumImpactGap: CFTimeInterval = 1.0 / 30
}
