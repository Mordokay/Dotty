import Foundation

/// This iPhone's copy of every voice note (spec: docs/superpowers/specs/2026-10-24-companion-recordings-
/// design.md): Documents/Recordings/<companion>/<id>.wav + <id>.json. Both transcripts stay here — Apple's
/// draft and Gemini's final — so the app can show them side by side.
struct LocalRecording: Codable, Identifiable, Hashable {
    var id: String          // Dotty's name, "20261024-153012"
    var companion: String
    var title: String
    var created: Date
    var duration: Double    // seconds
    var draft = ""          // Apple's on-device recognition
    var final = ""          // Gemini's, from the audio
    var summary = ""
    var points: [String] = []
    var needsFinal = false  // Gemini couldn't do it yet (quota, offline): tried again later
    var textSent = false    // Dotty has the final text (or the draft, while needsFinal)
}

enum RecordingStore {
    static var root: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("Recordings")
    }

    static func folder(_ companion: String) -> URL { root.appendingPathComponent(companion) }
    static func wavURL(_ r: LocalRecording) -> URL { folder(r.companion).appendingPathComponent("\(r.id).wav") }
    private static func jsonURL(companion: String, id: String) -> URL {
        folder(companion).appendingPathComponent("\(id).json")
    }

    static func recordings(companion: String) -> [LocalRecording] {
        let files = (try? FileManager.default.contentsOfDirectory(at: folder(companion), includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.pathExtension == "json" }
            .compactMap { try? JSONDecoder().decode(LocalRecording.self, from: Data(contentsOf: $0)) }
            .sorted { $0.created > $1.created }
    }

    static func find(companion: String, id: String) -> LocalRecording? {
        (try? Data(contentsOf: jsonURL(companion: companion, id: id))).flatMap { try? JSONDecoder().decode(LocalRecording.self, from: $0) }
    }

    static func save(_ r: LocalRecording) {
        try? FileManager.default.createDirectory(at: folder(r.companion), withIntermediateDirectories: true)
        if let data = try? JSONEncoder().encode(r) { try? data.write(to: jsonURL(companion: r.companion, id: r.id)) }
    }

    static func saveAudio(_ r: LocalRecording, wav: Data) {
        try? FileManager.default.createDirectory(at: folder(r.companion), withIntermediateDirectories: true)
        try? wav.write(to: wavURL(r))
    }

    static func delete(_ r: LocalRecording) {
        try? FileManager.default.removeItem(at: wavURL(r))
        try? FileManager.default.removeItem(at: jsonURL(companion: r.companion, id: r.id))
    }

    /// "20261024-153012" → that local date.
    static func date(fromName name: String) -> Date {
        let f = DateFormatter()
        f.dateFormat = "yyyyMMdd-HHmmss"
        f.locale = Locale(identifier: "en_US_POSIX")
        return f.date(from: name) ?? Date()
    }
}
