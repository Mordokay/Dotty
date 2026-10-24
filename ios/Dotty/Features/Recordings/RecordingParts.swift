import Foundation

/// A voice note arriving from Dotty while it's recorded (spec: docs/superpowers/specs/2026-10-24-companion-
/// recordings-design.md): parts (one per BOOT hold) of 16 kHz samples decoded from 0x1F 0x02 seq frames.
/// A missing sequence number marks a gap: the phone then asks Dotty for the whole file (rec.fetch).
struct RecordingParts {
    private(set) var parts: [[Int16]] = []
    private(set) var gap = false
    private var lastSeq: UInt8?

    var partCount: Int { parts.count }
    var samples: [Int16] { parts.flatMap { $0 } }

    mutating func startPart() { parts.append([]) }

    /// A fetch picking up where an interrupted one stopped: what came before, as one part.
    mutating func prefill(_ samples: [Int16]) {
        parts = [samples]
        lastSeq = nil
        gap = false
    }

    /// A raw value from Dotty: 0x1F 0x02 seq + an ADPCM frame (its own start state).
    mutating func add(_ frame: Data) {
        guard frame.count > 3, frame[frame.startIndex] == 0x1F, frame[frame.startIndex + 1] == 0x02 else { return }
        let seq = frame[frame.startIndex + 2]
        if let lastSeq, seq != lastSeq &+ 1 { gap = true }
        lastSeq = seq
        if parts.isEmpty { parts.append([]) }  // joined mid-recording: its start is missing anyway
        parts[parts.count - 1].append(contentsOf: Adpcm.decodeFrame(frame.dropFirst(3)))
    }

    mutating func undo() {
        if !parts.isEmpty { parts.removeLast() }
    }

    /// 16-bit mono PCM at 16 kHz as a WAV file.
    static func wav(_ samples: [Int16], rate: UInt32 = 16000) -> Data {
        var d = Data()
        func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        let bytes = UInt32(samples.count * 2)
        d.append(contentsOf: Array("RIFF".utf8)); u32(36 + bytes)
        d.append(contentsOf: Array("WAVEfmt ".utf8)); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16)
        d.append(contentsOf: Array("data".utf8)); u32(bytes)
        samples.withUnsafeBytes { d.append(contentsOf: $0) }
        return d
    }
}

/// When Gemini may be asked again for a recording's final transcript and summary: at once the first time,
/// then an hour later, then a day apart, never after 5 tries (a silent note, a refused file) — every try
/// re-uploads the audio and spends the free allowance.
enum RecordingRetry {
    static let maxAttempts = 5

    static func shouldTry(attempts: Int, last: Date?, now: Date) -> Bool {
        guard attempts < maxAttempts else { return false }
        guard attempts > 0, let last else { return true }
        let wait: TimeInterval = attempts == 1 ? 3600 : 86400
        return now.timeIntervalSince(last) >= wait
    }
}
