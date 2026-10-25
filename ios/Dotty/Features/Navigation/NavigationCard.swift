import SwiftUI

/// On the companion's page while a trip runs: where to, how far, when, and Stop.
struct NavigationCard: View {
    private let nav = NavigationService.shared

    var body: some View {
        if let trip = nav.trip {
            HStack(spacing: Spacing.m) {
                Image(systemName: trip.mode == .driving ? "car.fill" : trip.mode == .cycling ? "bicycle" : "figure.walk")
                    .font(.system(size: 22, weight: .semibold))
                    .foregroundStyle(DottyLight.firefly.color)
                VStack(alignment: .leading, spacing: 2) {
                    Text("\(verb(trip.mode)) to \(trip.destination)").font(.lpBody).foregroundStyle(Color.ink)
                    Text("\(distance(trip.left)) · arrive \(trip.eta.formatted(date: .omitted, time: .shortened))")
                        .font(.lpCaption).foregroundStyle(Color.inkFaint)
                }
                Spacer()
                Button("Stop") { nav.stop(why: "stopped in the app") }
                    .buttonStyle(.quiet(DottyLight.ember.color))
            }
            .padding(Spacing.l)
            .glassSurface(light: DottyLight.firefly.color, cornerRadius: Radius.card)
        }
    }

    private func verb(_ mode: NavMode) -> String {
        mode == .driving ? "Driving" : mode == .cycling ? "Cycling" : "Walking"
    }

    private func distance(_ m: Double) -> String {
        m < 1000 ? "\(Int(m)) m" : String(format: "%.1f km", m / 1000)
    }
}
