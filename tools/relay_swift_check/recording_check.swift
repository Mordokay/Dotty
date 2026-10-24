// Swift check for RecordingParts (ios/Dotty/Features/Recordings/RecordingParts.swift): a recording streamed
// from Dotty as parts (one per BOOT hold), each 0x1F 0x02 seq frame in order, a gap in seq noticed (→
// rec.fetch), undo dropping the last part.
// Run: cat ios/Dotty/Features/Relay/Adpcm.swift ios/Dotty/Features/Recordings/RecordingParts.swift \
//          tools/relay_swift_check/recording_check.swift > $TMP/r.swift && swiftc $TMP/r.swift -o $TMP/r && $TMP/r
import Foundation

var failed = 0
func check(_ name: String, _ ok: Bool) {
    print(ok ? "PASS \(name)" : "FAIL \(name)")
    if !ok { failed += 1 }
}

func frame(_ seq: UInt8, _ samples: [Int16]) -> Data {
    var state = Adpcm.State()
    return Data([0x1F, 0x02, seq]) + Adpcm.encodeFrame(samples[...], &state)
}

let ramp = (0..<570).map { Int16(($0 % 50) * 100) }
var parts = RecordingParts()
parts.startPart()
parts.add(frame(0, ramp))
parts.add(frame(1, ramp))
parts.startPart()
parts.add(frame(2, ramp))
check("frames in order build the parts", parts.partCount == 2 && parts.samples.count == 570 * 3 && !parts.gap)

var gappy = RecordingParts()
gappy.startPart()
gappy.add(frame(0, ramp))
gappy.add(frame(2, ramp))
check("a gap in seq is found (→ rec.fetch)", gappy.gap)

var undone = RecordingParts()
undone.startPart()
undone.add(frame(0, ramp))
undone.startPart()
undone.add(frame(1, ramp))
undone.add(frame(2, ramp))
undone.undo()
check("undo after late frames drops exactly the last part", undone.partCount == 1 && undone.samples.count == 570)
undone.startPart()
undone.add(frame(3, ramp))
check("a part after an undo carries on (no gap)", undone.partCount == 2 && undone.samples.count == 570 * 2 && !undone.gap)

let wav = RecordingParts.wav(Array(repeating: Int16(0), count: 16000))
check("a WAV of 1 s at 16 kHz", wav.count == 44 + 32000 && wav.prefix(4) == Data("RIFF".utf8))
exit(failed == 0 ? 0 : 1)
