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
var resumed = RecordingParts()
resumed.prefill(Array(repeating: Int16(7), count: 1140))
resumed.add(frame(5, ramp))
check("a resumed fetch keeps what came before", resumed.samples.count == 1140 + 570 && resumed.samples.first == 7 && !resumed.gap)

// iOS's recognizer starts its text over after a pause (the user's 56 s note kept only its last sentence).
var acc = DraftAccumulator()
acc.update("So people like to chew bubblegum")
acc.update("So people like to chew bubblegum but it is not healthy")
acc.update("I was thinking what else")
acc.update("I was thinking what else can you chew")
acc.update("So yeah I have to research this")
check("draft: text started over after a pause is kept", acc.text ==
      "So people like to chew bubblegum but it is not healthy I was thinking what else can you chew So yeah I have to research this")

let now = Date(timeIntervalSince1970: 1_800_000_000)
check("retry: the first try at once", RecordingRetry.shouldTry(attempts: 0, last: nil, now: now))
check("retry: the second after an hour", !RecordingRetry.shouldTry(attempts: 1, last: now.addingTimeInterval(-3000), now: now) &&
      RecordingRetry.shouldTry(attempts: 1, last: now.addingTimeInterval(-3700), now: now))
check("retry: then a day apart", !RecordingRetry.shouldTry(attempts: 2, last: now.addingTimeInterval(-80000), now: now) &&
      RecordingRetry.shouldTry(attempts: 2, last: now.addingTimeInterval(-90000), now: now))
check("retry: never after 5", !RecordingRetry.shouldTry(attempts: 5, last: now.addingTimeInterval(-9_000_000), now: now))
exit(failed == 0 ? 0 : 1)
