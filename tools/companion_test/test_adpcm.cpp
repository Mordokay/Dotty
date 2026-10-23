// uses:
// IMA ADPCM frames (the relay), each carrying its own decoder state (cartridges/companion/adpcm.h).
#include <cmath>
#include <cstdio>
#include <vector>

#include "adpcm.h"
#include "check.h"

int main() {
  std::vector<int16_t> pcm(570);
  for (size_t i = 0; i < pcm.size(); i++) pcm[i] = static_cast<int16_t>(8000 * std::sin(i * 2 * M_PI * 440 / 16000.0));
  uint8_t frame[adpcm::kHeader + 285];
  adpcm::State enc;
  const size_t bytes = adpcm::encodeFrame(pcm.data(), pcm.size(), enc, frame);
  CHECK_EQ("frame: header + 4 bits a sample", bytes, size_t(adpcm::kHeader + 285));
  std::vector<int16_t> out(570);
  const size_t samples = adpcm::decodeFrame(frame, bytes, out.data(), out.size());
  CHECK_EQ("decode: every sample back", samples, size_t(570));
  double err = 0, power = 0;
  for (size_t i = 0; i < 570; i++) err += std::pow(out[i] - pcm[i], 2), power += std::pow(pcm[i], 2);
  const double snr = 10 * std::log10(power / err);
  std::printf("snr %.1f dB\n", snr);
  CHECK("decode: close to the original (SNR > 20 dB)", snr > 20);
  // A second frame starts from where the first left off, but carries that state itself: decodable alone.
  uint8_t frame2[adpcm::kHeader + 285];
  adpcm::encodeFrame(pcm.data(), pcm.size(), enc, frame2);
  std::vector<int16_t> alone(570);
  CHECK("frame: decodable without the one before", adpcm::decodeFrame(frame2, bytes, alone.data(), 570) == 570);
  // The fixed pattern the app's Swift codec must match byte for byte.
  int16_t ramp[8] = {0, 1000, 2000, 3000, -3000, -2000, -1000, 0};
  adpcm::State s;
  uint8_t f[adpcm::kHeader + 4];
  adpcm::encodeFrame(ramp, 8, s, f);
  std::printf("pattern:");
  for (uint8_t b : f) std::printf(" %02x", b);
  std::printf("\n");
  CHECK("pattern: known header (predictor 0, index 0)", f[0] == 0 && f[1] == 0 && f[2] == 0);
  return failures ? 1 : 0;
}
