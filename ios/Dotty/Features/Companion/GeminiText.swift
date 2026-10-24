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
        try await generate(parts: [["text": prompt]], schema: schema)
    }

    /// The same with a WAV (a voice note): its audio inline, base64 (the request must stay under 20 MB).
    static func generateWithAudio(_ prompt: String, wav: Data, schema: [String: Any]) async throws -> [String: Any] {
        try await generate(parts: [["inline_data": ["mime_type": "audio/wav", "data": wav.base64EncodedString()]],
                                   ["text": prompt]], schema: schema, timeout: 180)
    }

    private static func generate(parts: [[String: Any]], schema: [String: Any], timeout: TimeInterval = 60) async throws -> [String: Any] {
        guard let key = GeminiKeys.load().first else { throw Failure.noKey }
        guard let url = URL(string: "https://generativelanguage.googleapis.com/v1beta/models/\(model):generateContent?key=\(key)") else {
            throw Failure.unreadable
        }
        var request = URLRequest(url: url, timeoutInterval: timeout)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        let body: [String: Any] = [
            "contents": [["parts": parts]],
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

    /// Character-sheet style, tested on the Partner (2026-10-23): the first version wrote like a fantasy
    /// novel ("Sprouted amidst the rich soil…", traits "earth-kissed, radiant") and replaced the user's
    /// own traits; the backstory drifted into "installed as software" until the opening said character.
    static let rules = """
        You help write the character sheet of a character (a person, animal or creature) who is the user's companion: \
        they talk every day. Write like a clear character sheet, not a novel: plain, concrete, everyday English; no \
        metaphors, no poetic or flowery language, no fantasy imagery. Third person, and call the person it lives with \
        "the user". The user's own words matter most: keep the facts and choices they wrote (romantic or adult ones \
        included) unless they contradict each other. Traits are plain words or short phrases for how it behaves \
        ("teasing", "patient", "blunt"). Quirks are concrete habits ("calls the user 'trouble'"). Way of speaking says \
        how it talks (sentence length, slang, how often it asks back) and how its feelings show in its voice. Backstory \
        is a few plain facts of the character's own life as they would tell it (where they grew up, what they did, how \
        they met the user); never about software, AI or devices. Don't give it a name: it gets one when it first wakes \
        up. Limits: personality up to 600 characters; up to 6 traits of up to 40 characters; up to 6 quirks of up to 60 \
        characters; way of speaking up to 300 characters; backstory up to 600 characters.
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
                      name: String = "", voice: String = "", avatar: String = "",
                      keep: [String] = []) -> (prompt: String, schema: [String: Any]) {
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
            prompt += "\nIts \(label) now:\n\(mine)\n\nRewrite its \(label) so it fits everything above. Keep the user's own " +
                "words exactly, never softening or toning them down; change only what is vague, clashes, or is written in " +
                "flowery language (rewrite that in plain words). For a list, keep the items that are fine word for word and " +
                "replace at most two. Don't repeat what the other fields already say."
        }
        if !keep.isEmpty {
            prompt += "\nThe user pinned these lines: keep each of them word for word, as its own line:\n" +
                keep.map { "- \($0)" }.joined(separator: "\n")
        }
        if field != "traits" && field != "quirks" { prompt += "\nOne sentence per line." }
        let list = field == "traits" || field == "quirks"
        let schema: [String: Any] = [
            "type": "OBJECT",
            "properties": ["value": list ? ["type": "ARRAY", "items": ["type": "STRING"]] : ["type": "STRING"]],
            "required": ["value"],
        ]
        return (prompt, schema)
    }

    /// ✨ for a macro's prompt: improve what's written (or write one from the name and phrases). The
    /// prompt goes to a text model that writes ~150 words for the companion to read aloud.
    static func macro(name: String, triggers: String, current: String) -> (prompt: String, schema: [String: Any]) {
        var prompt = """
            You improve a prompt for a text model. That model writes about 300 words (two minutes read aloud) for a \
            desk companion to say to the user, in the companion's own voice and character, which it is given \
            separately. Write the prompt in plain, direct English, in the second person to the companion, the way \
            the user would ask ("Tell me...", "Guide me..."): the tone, what to include and in what order, how to \
            pace it (pauses written as ...), how to end. Keep the user's intent and their own words where they \
            work; make vague parts concrete. It must stay within Google's content rules: romantic, flirty, teasing \
            or suggestive is fine, never sexually explicit (no sexual acts, nudity or body parts). At most 1800 \
            characters. Only the prompt.
            """
        prompt += "\n\nMacro name: \(name.isEmpty ? "(none yet)" : name)"
        prompt += "\nThe user starts it by saying: \(triggers.split(separator: "\n").joined(separator: " / "))"
        if current.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            prompt += "\n\nThere is no prompt yet: write one that fits the name and the phrases."
        } else {
            prompt += "\n\nThe prompt now:\n\(current)\n\nRewrite it better."
        }
        let schema: [String: Any] = [
            "type": "OBJECT", "properties": ["value": ["type": "STRING"]], "required": ["value"],
        ]
        return (prompt, schema)
    }
}
