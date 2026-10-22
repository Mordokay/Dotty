import Foundation

/// Gemini's text API from the app (the companion co-designer): one request, a JSON answer shaped by
/// a response schema. Uses the first key in the Keychain. Flash-Lite: ~2 s, where the bigger Flash
/// models took 20-60 s thinking (measured 2026-10-23).
enum GeminiText {
    static let model = "gemini-flash-lite-latest"

    enum Failure: LocalizedError {
        case noKey, refused(String), offline, unreadable
        var errorDescription: String? {
            switch self {
            case .noKey: "Add a Gemini key in Companion settings (the gear) first."
            case .refused(let why): "Gemini said no: \(why)"
            case .offline: "Couldn't reach Gemini. Check the iPhone's connection."
            case .unreadable: "Gemini's answer didn't make sense. Try again."
            }
        }
    }

    static func generate(_ prompt: String, schema: [String: Any]) async throws -> [String: Any] {
        guard let key = GeminiKeys.load().first else { throw Failure.noKey }
        guard let url = URL(string: "https://generativelanguage.googleapis.com/v1beta/models/\(model):generateContent?key=\(key)") else {
            throw Failure.unreadable
        }
        var request = URLRequest(url: url, timeoutInterval: 60)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        let body: [String: Any] = [
            "contents": [["parts": [["text": prompt]]]],
            "generationConfig": ["responseMimeType": "application/json", "responseSchema": schema],
        ]
        request.httpBody = try JSONSerialization.data(withJSONObject: body)
        let data: Data, response: URLResponse
        do {
            (data, response) = try await URLSession.shared.data(for: request)
        } catch {
            throw Failure.offline
        }
        let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
        guard (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw Failure.refused((json["error"] as? [String: Any])?["message"] as? String ?? "no answer")
        }
        guard let text = (((json["candidates"] as? [[String: Any]])?.first?["content"] as? [String: Any])?["parts"]
                  as? [[String: Any]])?.first?["text"] as? String,
              let answer = (try? JSONSerialization.jsonObject(with: Data(text.utf8))) as? [String: Any] else {
            throw Failure.unreadable
        }
        return answer
    }
}

/// What Gemini is told when it designs (or redesigns part of) a companion.
enum CompanionDesign {
    static let avatars = ["robot", "dog", "cat", "fox", "owl", "bear", "girl", "guy", "ghost", "dragon"]

    static var voiceList: String {
        VoicesView.voices.map { "\($0.name) (\($0.style))" }.joined(separator: ", ")
    }

    static let rules = """
        You design characters for Dotty, a small desk companion people talk to by voice. Write in English, in the \
        third person ("A cheerful little dragon who…", never "I am…"), and call the person it lives with "the user". \
        Don't give it a name: it gets one when it first wakes up. Limits: personality up to 600 characters; up to 6 \
        traits of up to 40 characters; up to 6 quirks of up to 60 characters; way of speaking up to 300 characters \
        (how it talks, and how it shows its feelings in its voice); backstory up to 600 characters.
        """

    static func everything(_ description: String) -> (prompt: String, schema: [String: Any]) {
        let prompt = rules + " Pick the voice that suits it best from: \(voiceList); and the closest avatar from: " +
            avatars.joined(separator: ", ") + ".\n\nDescription: \(description)"
        let schema: [String: Any] = [
            "type": "OBJECT",
            "properties": [
                "personality": ["type": "STRING"], "traits": ["type": "ARRAY", "items": ["type": "STRING"]],
                "quirks": ["type": "ARRAY", "items": ["type": "STRING"]], "speaking": ["type": "STRING"],
                "backstory": ["type": "STRING"], "voice": ["type": "STRING", "enum": VoicesView.voices.map(\.name)],
                "voiceReason": ["type": "STRING"], "avatar": ["type": "STRING", "enum": avatars],
            ],
            "required": ["personality", "traits", "quirks", "speaking", "backstory", "voice", "voiceReason", "avatar"],
        ]
        return (prompt, schema)
    }

    /// One field again, with the whole character in mind: every filled field (the one being
    /// rewritten too, as the version to improve on), the name, the voice and the avatar.
    static func field(_ field: String, label: String, description: String, current: [String: String],
                      name: String = "", voice: String = "", avatar: String = "") -> (prompt: String, schema: [String: Any]) {
        let labels = ["personality": "Personality", "traits": "Traits", "quirks": "Quirks",
                      "speaking": "Way of speaking", "backstory": "Backstory"]
        var known: [String] = []
        if !name.isEmpty { known.append("Name: \(name)") }
        if !voice.isEmpty {
            let style = VoicesView.voices.first { $0.name == voice }?.style ?? ""
            known.append("Voice: \(voice)\(style.isEmpty ? "" : " (\(style))")")
        }
        if !avatar.isEmpty && avatar != "photo" { known.append("Looks like: a \(avatar)") }
        for key in ["personality", "traits", "quirks", "speaking", "backstory"] where key != field {
            if let value = current[key], !value.isEmpty { known.append("\(labels[key] ?? key): \(value)") }
        }
        let mine = current[field] ?? ""
        var prompt = rules + "\n\n"
        if !description.isEmpty { prompt += "Description: \(description)\n" }
        if !known.isEmpty { prompt += "The character so far:\n" + known.joined(separator: "\n") + "\n" }
        if mine.isEmpty {
            prompt += "\nWrite its \(label), fitting everything above."
        } else {
            prompt += "\nIts \(label) now:\n\(mine)\n\nRewrite its \(label) so it fits everything above: keep what works, " +
                "make it fresher and more vivid, and don't repeat what the other fields already say."
        }
        let list = field == "traits" || field == "quirks"
        let schema: [String: Any] = [
            "type": "OBJECT",
            "properties": ["value": list ? ["type": "ARRAY", "items": ["type": "STRING"]] : ["type": "STRING"]],
            "required": ["value"],
        ]
        return (prompt, schema)
    }
}
