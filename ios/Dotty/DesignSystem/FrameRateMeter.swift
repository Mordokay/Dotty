import QuartzCore
import SwiftUI

/// Counts the frames the app actually draws per second (debug builds), to keep an eye on what
/// each effect costs on a real phone.
@MainActor
@Observable
final class FrameRateMeter: NSObject {
    private(set) var framesPerSecond = 0

    @ObservationIgnored private var link: CADisplayLink?
    @ObservationIgnored private var frames = 0
    @ObservationIgnored private var windowStart: CFTimeInterval = 0

    func start() {
        guard link == nil else { return }
        let link = CADisplayLink(target: self, selector: #selector(tick(_:)))
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    func stop() {
        link?.invalidate()
        link = nil
    }

    @objc private func tick(_ link: CADisplayLink) {
        if windowStart == 0 { windowStart = link.timestamp }
        frames += 1
        let elapsed = link.timestamp - windowStart
        if elapsed >= 0.5 {
            framesPerSecond = Int((Double(frames) / elapsed).rounded())
            frames = 0
            windowStart = link.timestamp
        }
    }
}

/// A small frames-per-second readout for the corner of the screen.
struct FrameRateBadge: View {
    @State private var meter = FrameRateMeter()

    var body: some View {
        Text("\(meter.framesPerSecond) fps")
            .font(.system(size: 12, weight: .bold, design: .rounded).monospacedDigit())
            .foregroundStyle(meter.framesPerSecond >= 50 ? DottyLight.leaf.color : meter.framesPerSecond >= 30 ? DottyLight.amber.color : DottyLight.ember.color)
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(Capsule().fill(Color.voidRaised.opacity(0.85)))
            .onAppear { meter.start() }
            .onDisappear { meter.stop() }
            .allowsHitTesting(false)
            .accessibilityHidden(true)
    }
}
