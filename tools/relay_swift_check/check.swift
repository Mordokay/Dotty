// The relay's pure Swift parts, checked from the command line (no Xcode tests in this project):
//   cat ios/Dotty/Features/Relay/Adpcm.swift [ios/Dotty/Features/Relay/RelayMessages.swift] \
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
exit(failed == 0 ? 0 : 1)
