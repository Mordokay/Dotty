import Foundation

/// IMA ADPCM for the relay, the twin of the firmware's
/// cartridges/companion/adpcm.h — frames carry their start state (predictor int16 LE + index), two
/// samples a byte, low nibble first. Must match byte for byte.
enum Adpcm {
    static let header = 3
    static let steps: [Int32] = [
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
        4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
        22385, 24623, 27086, 29794, 32767,
    ]
    static let indexStep: [Int] = [-1, -1, -1, -1, 2, 4, 6, 8]

    struct State {
        var predictor: Int32 = 0
        var index = 0
    }

    @inline(__always) static func decode(_ code: UInt8, _ s: inout State) -> Int16 {
        let step = steps[s.index]
        var diff = step >> 3
        if code & 4 != 0 { diff += step }
        if code & 2 != 0 { diff += step >> 1 }
        if code & 1 != 0 { diff += step >> 2 }
        s.predictor = max(-32768, min(32767, code & 8 != 0 ? s.predictor - diff : s.predictor + diff))
        s.index = max(0, min(88, s.index + indexStep[Int(code & 7)]))
        return Int16(s.predictor)
    }

    @inline(__always) static func encode(_ sample: Int16, _ s: inout State) -> UInt8 {
        let step = steps[s.index]
        var diff = Int32(sample) - s.predictor
        var code: UInt8 = 0
        if diff < 0 { code = 8; diff = -diff }
        if diff >= step { code |= 4; diff -= step }
        if diff >= step >> 1 { code |= 2; diff -= step >> 1 }
        if diff >= step >> 2 { code |= 1 }
        _ = decode(code, &s)
        return code
    }

    static func encodeFrame(_ pcm: ArraySlice<Int16>, _ s: inout State) -> Data {
        var out = Data(capacity: header + (pcm.count + 1) / 2)
        out.append(UInt8(truncatingIfNeeded: s.predictor))
        out.append(UInt8(truncatingIfNeeded: s.predictor >> 8))
        out.append(UInt8(s.index))
        var i = pcm.startIndex
        while i < pcm.endIndex {
            let lo = encode(pcm[i], &s)
            let hi: UInt8 = i + 1 < pcm.endIndex ? encode(pcm[i + 1], &s) : 0
            out.append(lo | hi << 4)
            i += 2
        }
        return out
    }

    static func decodeFrame(_ frame: Data) -> [Int16] {
        let bytes = [UInt8](frame)
        guard bytes.count >= header else { return [] }
        var s = State(predictor: Int32(Int16(bitPattern: UInt16(bytes[0]) | UInt16(bytes[1]) << 8)), index: min(88, Int(bytes[2])))
        var out: [Int16] = []
        out.reserveCapacity((bytes.count - header) * 2)
        for b in bytes[header...] {
            out.append(decode(b & 0x0F, &s))
            out.append(decode(b >> 4, &s))
        }
        return out
    }
}
