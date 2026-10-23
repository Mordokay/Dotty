// The relay's pure Swift parts, checked from the command line (no Xcode tests in this project):
//   cat ios/Dotty/Features/Relay/Adpcm.swift ios/Dotty/Features/Relay/RelayMessages.swift \
//       tools/relay_swift_check/check.swift > /tmp/c.swift && swiftc /tmp/c.swift -o /tmp/c && /tmp/c
// The ADPCM pattern must equal the firmware's (tools/companion_test/test_adpcm.cpp): 00 00 00 70 77 ff 5f.
import Foundation

var failed = 0
func check(_ name: String, _ ok: Bool) {
    print("\(ok ? "PASS" : "FAIL") \(name)")
    if !ok { failed += 1 }
}

var state = Adpcm.State()
let ramp: [Int16] = [0, 1000, 2000, 3000, -3000, -2000, -1000, 0]
let frame = Adpcm.encodeFrame(ramp[...], &state)
let hex = frame.map { String(format: "%02x", $0) }.joined(separator: " ")
print("pattern: \(hex)")
check("adpcm: the firmware's pattern", hex == "00 00 00 70 77 ff 5f")
check("adpcm: decodes 8 samples", Adpcm.decodeFrame(frame).count == 8)
// Gemini's messages lose their audio (sent to Dotty as ADPCM), everything else stays.
let pcmA = Data([1, 0, 2, 0]), pcmB = Data([3, 0])
let message: [String: Any] = ["serverContent": ["modelTurn": ["parts": [
    ["inlineData": ["mimeType": "audio/pcm;rate=24000", "data": pcmA.base64EncodedString()]],
    ["text": "hi"],
    ["inlineData": ["mimeType": "audio/pcm;rate=24000", "data": pcmB.base64EncodedString()]],
]]]]
let stripped = RelayMessages.stripAudio(try! JSONSerialization.data(withJSONObject: message))
let back = (try? JSONSerialization.jsonObject(with: stripped.json)) as? [String: Any]
let parts = ((back?["serverContent"] as? [String: Any])?["modelTurn"] as? [String: Any])?["parts"] as? [[String: Any]]
check("strip: the audio parts out, the rest kept", parts?.count == 1 && parts?.first?["text"] as? String == "hi")
check("strip: the audio in order", stripped.audio == [pcmA, pcmB])
let plain = Data(#"{"setupComplete":{}}"#.utf8)
check("strip: no audio, untouched", RelayMessages.stripAudio(plain).json == plain && RelayMessages.stripAudio(plain).audio.isEmpty)

// JSON to Dotty in 0x02 pieces (flags bit 0 = more follows).
let json = Data((0..<700).map { UInt8(65 + $0 % 26) })
let pieces = RelayMessages.pieces(json, max: 290)
check("pieces: 700 bytes at 290 a piece = 3", pieces.count == 3)
check("pieces: headers 0x02 + more, more, last", pieces.map { [$0[0], $0[1]] } == [[2, 1], [2, 1], [2, 0]])
check("pieces: each fits", pieces.allSatisfy { $0.count <= 290 })
check("pieces: join back exactly", pieces.reduce(Data()) { $0 + $1.dropFirst(2) } == json)
exit(failed == 0 ? 0 : 1)
