import CoreMotion
import QuartzCore
import SwiftUI

/// The phone's tilt from the gyroscope, for parallax. One shared reader for the whole app.
/// The neutral pose slowly follows how the person holds the phone, so only tilting moves things.
/// Without motion hardware (the Simulator) the tilt stays at zero.
@MainActor
final class MotionTilt {
    static let shared = MotionTilt()

    private let manager = CMMotionManager()
    private var neutral: SIMD2<Double>?
    private var smoothed = SIMD2<Double>(0, 0)
    private var users = 0
    private var lastSample: CFTimeInterval = 0

    func start() {
        users += 1
        guard users == 1, manager.isDeviceMotionAvailable else { return }
        manager.deviceMotionUpdateInterval = 1.0 / 60.0
        manager.startDeviceMotionUpdates()
    }

    func stop() {
        users = max(0, users - 1)
        if users == 0 { manager.stopDeviceMotionUpdates() }
    }

    /// The current tilt, about -1...1 on each axis. Views can call it every frame: within a
    /// frame it returns the same value, so smoothing doesn't speed up with more callers.
    func sample() -> CGPoint {
        let now = CACurrentMediaTime()
        guard now - lastSample > 0.004 else { return CGPoint(x: smoothed.x, y: smoothed.y) }
        lastSample = now
        if let gravity = manager.deviceMotion?.gravity {
            let current = SIMD2(gravity.x, gravity.y)
            let base = neutral ?? current
            neutral = base + (current - base) * 0.004
            let target = (current - base) * 2.6
            let clamped = SIMD2(min(max(target.x, -1), 1), min(max(target.y, -1), 1))
            smoothed += (clamped - smoothed) * 0.18
        }
        return CGPoint(x: smoothed.x, y: smoothed.y)
    }

    /// How far a layer at `depth` shifts for this tilt.
    static func shift(_ tilt: CGPoint, depth: CGFloat) -> CGSize {
        CGSize(width: tilt.x * (1 - depth) * Depth.tiltReach, height: tilt.y * (1 - depth) * Depth.tiltReach)
    }
}
