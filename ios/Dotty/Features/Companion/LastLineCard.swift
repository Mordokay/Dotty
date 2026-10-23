import SwiftUI

/// The last thing the companion on Dotty said, replayed on Dotty's speaker: play / pause and a loop
/// switch (`companion.replay`; Dotty keeps the line per companion as last.wav on its card). Dotty
/// tells about changes made on it with the `companion.replay` event.
struct LastLineCard: View {
    let name: String
    @Environment(DottyLink.self) private var link
    @State private var text = ""
    @State private var seconds = 0.0
    @State private var when: Date?
    @State private var state = "stopped"
    @State private var loop = false
    @State private var available = false
    @State private var loaded = false
    @State private var busy = false
    @State private var error: String?

    var body: some View {
        GlassCard(title: "Last thing \(name) said") {
            VStack(alignment: .leading, spacing: Spacing.m) {
                if loaded && text.isEmpty {
                    Text("Nothing yet. Talk to \(name) on Dotty and the last reply shows up here.")
                        .font(.lpCallout).foregroundStyle(Color.inkMuted)
                } else if !text.isEmpty {
                    Text("\u{201C}\(text)\u{201D}").font(.lpBody).foregroundStyle(Color.ink)
                        .fixedSize(horizontal: false, vertical: true)
                    HStack(spacing: Spacing.m) {
                        Button { Task { await send(["action": "toggle"]) } } label: {
                            Image(systemName: state == "playing" ? "pause.fill" : "play.fill")
                                .font(.system(size: 20, weight: .semibold))
                                .frame(width: 48, height: 48)
                                .background(Circle().fill(DottyLight.firefly.color))
                                .foregroundStyle(Color.onLight)
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(state == "playing" ? "Pause" : "Play on Dotty")
                        Button { Task { await send(["loop": !loop]) } } label: {
                            Image(systemName: "repeat")
                                .font(.system(size: 17, weight: .semibold))
                                .frame(width: 40, height: 40)
                                .background(Circle().fill(loop ? DottyLight.firefly.color.opacity(0.3) : Color.glassStrong))
                                .overlay(Circle().strokeBorder(loop ? DottyLight.firefly.color : Color.glassEdge, lineWidth: loop ? 2 : 1))
                                .foregroundStyle(loop ? DottyLight.firefly.color : Color.inkMuted)
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(loop ? "Loop on" : "Loop off")
                        VStack(alignment: .leading, spacing: 2) {
                            Text(caption).font(.lpCaption).foregroundStyle(Color.inkMuted)
                            if let when {
                                Text(when.formatted(.relative(presentation: .named))).font(.lpCaption).foregroundStyle(Color.inkFaint)
                            }
                        }
                        Spacer()
                        if busy { ProgressView().controlSize(.small) }
                    }
                    .disabled(!available || busy)
                    .opacity(available ? 1 : 0.5)
                    if !available {
                        Text("Dotty is busy talking or in text-only mode.").font(.lpCaption).foregroundStyle(Color.inkFaint)
                    }
                }
                if let error { Text(error).font(.lpCaption).foregroundStyle(DottyLight.ember.color) }
            }
            .padding(Spacing.m)
        }
        .needsDotty(link)
        .task { await send([:]) }
        .onChange(of: link.connection) { _, state in
            if state == .connected { Task { await send([:]) } }
        }
        .onChange(of: link.eventCount) { _, _ in
            guard let event = link.lastEvent else { return }
            if event.event == "companion.replay", let r = event["replay"] as? [String: Any] {
                apply(replay: r)
            } else if event.event == "companion.changed" {
                Task { await send([:]) }
            }
        }
    }

    private var caption: String {
        let length = seconds < 1 ? "" : " · \(Int(seconds.rounded())) s"
        switch state {
        case "playing": return (loop ? "Playing on repeat" : "Playing on Dotty") + length
        case "paused": return "Paused" + length
        default: return (loop ? "Plays on repeat" : "Plays once") + length
        }
    }

    private func apply(replay r: [String: Any]) {
        state = r["state"] as? String ?? "stopped"
        loop = r["loop"] as? Bool ?? false
        available = r["available"] as? Bool ?? false
    }

    /// A replay command (an empty one just reads the state); the reply has the line and the state.
    private func send(_ args: [String: Any]) async {
        guard link.connection == .connected else { return }
        if !args.isEmpty { busy = true }
        defer { busy = false }
        do {
            let r = try await link.send("companion.replay", args)
            if let last = r["last"] as? [String: Any] {
                text = last["text"] as? String ?? ""
                seconds = last["seconds"] as? Double ?? 0
                // Dotty's clock is local: its epoch seconds are local time.
                when = (last["t"] as? Int).map { Date(timeIntervalSince1970: TimeInterval($0 - TimeZone.current.secondsFromGMT())) }
            } else {
                text = ""
            }
            if let replay = r["replay"] as? [String: Any] { apply(replay: replay) }
            loaded = true
            error = nil
        } catch {
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}
