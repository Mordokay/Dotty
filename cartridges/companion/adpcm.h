#pragma once

#include <stddef.h>
#include <stdint.h>

// IMA ADPCM (the relay through the iPhone: voice up, reply down), 4 bits a sample (4:1 on 16-bit PCM).
// A frame = predictor (int16, little-endian) + step index (uint8) + two samples a byte (low nibble
// first): it carries the state it starts from, so a lost frame doesn't garble the next. The app's
// Swift codec (Adpcm.swift) must match byte for byte.
namespace adpcm {

constexpr size_t kHeader = 3;

struct State {
  int32_t predictor = 0;
  int index = 0;
};

constexpr int16_t kSteps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kIndexStep[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

inline int clampIndex(int i) { return i < 0 ? 0 : i > 88 ? 88 : i; }
inline int32_t clamp16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

inline int16_t decodeNibble(uint8_t code, State &s) {
  const int step = kSteps[s.index];
  int32_t diff = step >> 3;
  if (code & 4) diff += step;
  if (code & 2) diff += step >> 1;
  if (code & 1) diff += step >> 2;
  s.predictor = clamp16(code & 8 ? s.predictor - diff : s.predictor + diff);
  s.index = clampIndex(s.index + kIndexStep[code & 7]);
  return static_cast<int16_t>(s.predictor);
}

inline uint8_t encodeSample(int16_t sample, State &s) {
  const int step = kSteps[s.index];
  int32_t diff = sample - s.predictor;
  uint8_t code = 0;
  if (diff < 0) code = 8, diff = -diff;
  if (diff >= step) code |= 4, diff -= step;
  if (diff >= step >> 1) code |= 2, diff -= step >> 1;
  if (diff >= step >> 2) code |= 1;
  decodeNibble(code, s);  // the decoder's state, so both stay in step
  return code;
}

// count samples → out (kHeader + (count + 1) / 2 bytes); returns the bytes written.
inline size_t encodeFrame(const int16_t *pcm, size_t count, State &s, uint8_t *out) {
  out[0] = static_cast<uint8_t>(s.predictor & 0xFF);
  out[1] = static_cast<uint8_t>((s.predictor >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>(s.index);
  size_t n = kHeader;
  for (size_t i = 0; i < count; i += 2) {
    const uint8_t lo = encodeSample(pcm[i], s);
    const uint8_t hi = i + 1 < count ? encodeSample(pcm[i + 1], s) : 0;
    out[n++] = static_cast<uint8_t>(lo | hi << 4);
  }
  return n;
}

// A frame → samples (at most max); returns how many.
inline size_t decodeFrame(const uint8_t *in, size_t len, int16_t *out, size_t max) {
  if (len < kHeader) return 0;
  State s;
  s.predictor = static_cast<int16_t>(in[0] | in[1] << 8);
  s.index = clampIndex(in[2]);
  size_t n = 0;
  for (size_t i = kHeader; i < len && n < max; i++) {
    out[n++] = decodeNibble(in[i] & 0x0F, s);
    if (n < max) out[n++] = decodeNibble(in[i] >> 4, s);
  }
  return n;
}

}  // namespace adpcm
