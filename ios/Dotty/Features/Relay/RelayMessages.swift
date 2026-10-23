import Foundation

/// The relay's pure parts (checked by tools/relay_swift_check): Gemini's messages go to Dotty without
/// their audio (the audio goes as ADPCM frames), and JSON goes to Dotty's Data characteristic in pieces:
/// 0x02, flags (bit 0 = more follows), then the bytes — the firmware's PieceAssembler.
enum RelayMessages {
    /// The message without its `inlineData` parts, and their audio (raw PCM) in order. A message with no
    /// audio comes back untouched.
    static func stripAudio(_ json: Data) -> (json: Data, audio: [Data]) {
        guard var root = (try? JSONSerialization.jsonObject(with: json)) as? [String: Any],
              var content = root["serverContent"] as? [String: Any],
              var turn = content["modelTurn"] as? [String: Any],
              let parts = turn["parts"] as? [[String: Any]] else { return (json, []) }
        var audio: [Data] = []
        var kept: [[String: Any]] = []
        for part in parts {
            if let inline = part["inlineData"] as? [String: Any], let b64 = inline["data"] as? String,
               let pcm = Data(base64Encoded: b64) {
                audio.append(pcm)
            } else {
                kept.append(part)
            }
        }
        guard !audio.isEmpty else { return (json, []) }
        turn["parts"] = kept
        content["modelTurn"] = turn
        root["serverContent"] = content
        let out = (try? JSONSerialization.data(withJSONObject: root, options: [.withoutEscapingSlashes])) ?? json
        return (out, audio)
    }

    /// `json` in pieces of at most `max` bytes each, header included.
    static func pieces(_ json: Data, max: Int) -> [Data] {
        let room = Swift.max(1, max - 2)
        var out: [Data] = []
        var at = json.startIndex
        repeat {
            let end = json.index(at, offsetBy: room, limitedBy: json.endIndex) ?? json.endIndex
            var piece = Data([0x02, end < json.endIndex ? 1 : 0])
            piece.append(json[at..<end])
            out.append(piece)
            at = end
        } while at < json.endIndex
        return out
    }
}
